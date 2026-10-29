# G1CFW private protocol (command byte `0xF0`)

Injected by `cfw/src/g1cfw.c` on Even G1 application 1.5.60. The stock BLE
receive entry at `0x23378` calls `spec_ble_command_hook(data, len)` and only
forwards a packet to the request dispatcher when that hook returns non-zero;
the profile redirects the call to `cfw_rx`, which consumes `0xF0` packets and
passes everything else through unchanged. Each temple handles its own packets,
so a phone must send display commands to both the `_L_` and `_R_` connections.

Packets travel over the Nordic UART RX characteristic exactly like stock
commands. The stock receive buffer holds **at most 256 bytes** per packet. Every
command is answered with one notification on the TX characteristic:

```
request:  F0 <sub> <payload...>
reply:    F0 <sub> <status> <body...>
```

| status | meaning |
| --- | --- |
| `00` | ok |
| `01` | bad length (payload shorter than the command needs) |
| `02` | unknown sub-command (reply `sub` echoes the request) |
| `03` | bad rectangle (odd x or width, zero size, or outside 640x200) |
| `04` | panel off (frame written where applicable, nothing pushed) |
| `05` | bad sequence (stream packet out of order or no message in flight) |
| `06` | bad CRC (assembled message does not match the declared CRC-16) |
| `07` | too big (declared length over 4096, or more data than declared) |
| `08` | bad record (record header or length does not fit the message) |
| `09` | bad RLE (PackBits data ended before the rectangle was filled) |
| `0A` | bad font (TEXT font other than 0, 1, 2) |

A bare `F0` with no sub-command is answered `F0 FF 01`.

Little-endian 16-bit fields are written `u16`. The frame is 640x200 at 4 bits
per pixel, two pixels per byte, high nibble on the left, 320 bytes per row.
Rectangles are `[x0:u16][y0:u16][w:u16][h:u16]` with even `x0` and `w`.

## `00` PING

Reply body: `"G1CFW/<n>" 00 <major> <minor> <patch>` where `<n>` is the
protocol revision (11 for this document) and the version is the stock base
(1, 5, 60). `g1flash.py ping` checks this.

## `01` BLIT: `[flags][rect][pixels]`

Writes pixels into the frame buffer.

- `flags` bit 0: input is 1 bit per pixel, MSB first, each row padded to a
  whole byte (`(w+7)/8` bytes per row). Set bits become the grey level in the
  high nibble of `flags` (0 means 15); clear bits become 0.
- `flags` bit 0 clear: input is raw 4bpp rows of `w/2` bytes.
- `flags` bit 1: push the rectangle to the panel after writing.
- `flags` bit 2: the pixel bytes are PackBits-encoded (`n` 0-127: `n+1`
  literal bytes follow; `n` 129-255: repeat the next byte `257-n` times; 128
  is a no-op). The decoder stops after `rows * stride` output bytes.

The payload must contain at least `rows * stride` pixel bytes. With the 256-byte
packet limit, a 640-wide 1bpp blit carries three rows per packet; a full-frame
1bpp image is 67 packets.

## `02` FILL: `[flags][rect][level]`

Fills the rectangle with `level & 0xF`. `flags` bit 1 pushes it afterwards.

## `03` FLUSH: `[rect]`

Pushes the rectangle from the frame buffer to the panel using the stock
`_reflash_fb_data_to_lcd` path with the stock column/row origins. The stock
routine's x1 is exclusive (it sends `x1/2 - x0/2` bytes per row); the CFW
passes `x0 + w`, so the full width including columns 638-639 is pushed. The
stock full-frame refresh passes 639 and clips those two columns, which is
visible as a missing right border on stock screens.

## `04` INFO

Reply body: `[powered][max_packet][col_off:i16][row_off:i16][frame_ptr:u32][width/4][height]`.
`max_packet` is 255; `width/4` is 160.

## `05` PANEL: `[mode]`

`mode` 1 resumes the panel through the stock `global_system_resume` path, the
same entry the stock silent-mode exit uses; 0 suspends it through
`global_system_suspend`. Both are no-ops unless the power state actually
changes, and the stock resume only re-powers the panel when its wear/dock
policy allows it. `mode` 2 instead calls the panel context's own `on`
callback (`panel_on`: rail on, SPI init, panel init, powered flag set) and 3
its `off` callback, bypassing that policy; use these to draw while docked. Reply body: `[power_state][powered]`, where `power_state` is the stock
byte (0 running, 1 suspended, 2 suspending, 3 resuming) and `powered` is the
panel context flag that FLUSH checks. While the glasses are docked the stock
firmware keeps the panel suspended, so a PANEL on is needed before anything
becomes visible there.

## `06` TEXT: `[font][flags][x0:u16][y0:u16][x1:u16][y1:u16][max_lines][utf8...]`

Draws left-aligned text with the stock font engine (`gui_utf_draw`) into the
box, wrapping at `x1` and stopping after `max_lines` lines or when the next
line would pass `y1`. Font 0 is the proportional 20 px face (26-row cells,
27 px line pitch; ASCII from internal flash, other scripts fetched from the
external flash at draw time); fonts 1 and 2 are the stock digit faces, 32 and
39 rows tall. `flags` bit 0 clears the box to black first, bit 1 pushes the
box afterwards. `x0` and `x1` must be even, and `y1` plus the cell height
must stay within 200 rows (so `y1 <= 173` for font 0). Text is 1-240 bytes;
`\n` moves to the next line. Glyphs are only on or off; there is no grey
level. TEXT is also a valid stream record.

## `07` COPY: `[flags][src rect][dst x:u16][dst y:u16]`

Copies a rectangle to another position in the frame buffer. Overlapping
source and destination are handled, so it can scroll a region by any offset.
Destination x must be even and the rectangle must fit. `flags` bit 1 pushes
the destination rectangle afterwards. COPY is also a valid stream record.

## `10` STREAM: `[seq][flags][data]`

Multi-packet messages with one reply per message, so the phone can write
packets back to back. `flags` bit 0 marks the first packet, whose data begins
with `[total:u16][crc:u16]` (CRC-16/CCITT-FALSE over the whole message); bit 1
marks the last. `seq` increments by one per packet; a gap, a packet with no
message in flight, a declared length over 4096, or more data than declared is
refused with status `05`/`07` and the message is dropped. Mid-message packets
get no reply. The last packet is answered `F0 10 <status> <seq> <records done>`
after the message runs.

A message is a list of records `[cmd:u8][len:u16][payload]` using the BLIT,
FILL, FLUSH, TEXT and COPY payloads above, so one message can carry a clear, several
compressed bands and a single flush. The first failing record stops the
message; `records done` says how many ran.

## `11` STATS

Reply body: `[messages:u16][errors:u16][stock_swallowed:u16][last_status]
[message_active][message_bytes:u16][message_max>>8][ram>>8]`.

## Stock commands the CFW swallows

`0x15` (BMP data), `0x16` (BMP CRC) and `0x20` (BMP end) are consumed without
reply: their staging RAM at `0x200174e4` (5,120 bytes) now holds the CFW
context and the 4 KB message buffer.

## Caveats

- From G1CFW/8 the stock UI refresh pipeline (`display_reflash_handler`) and
  the whole-frame clear are stubbed, so the stock display thread never writes
  the frame buffer or panel again. The CFW owns the frame; stock screens no
  longer draw.
- Flushing runs on the BLE worker thread and takes about 1 ms plus SPI time
  per call. Prefer one FLUSH per frame over flushing every BLIT.
- Single-packet commands have no CRC; their reply is the acknowledgement.
  STREAM messages are CRC-checked and are the pipelined path.
