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
| `0B` | no memory (heap arena unavailable, directory or arena full) |
| `0C` | no asset (unknown id) |
| `0D` | bad asset (wrong kind, incomplete image, append to a closed asset) |
| `0E` | no object (SHOW or drop named an unknown object id) |

A bare `F0` with no sub-command is answered `F0 FF 01`.

Little-endian 16-bit fields are written `u16`. The frame is 640x200 at 4 bits
per pixel, two pixels per byte, high nibble on the left, 320 bytes per row.
Rectangles are `[x0:u16][y0:u16][w:u16][h:u16]` with even `x0` and `w`.

## `00` PING

Reply body: `"G1CFW/<n>" 00 <major> <minor> <patch>` where `<n>` is the
protocol revision (18 for this document) and the version is the stock base
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

## Assets (`08`-`0C`)

Assets live in an arena taken from the stock heap on first use (48 KB is
requested first, falling back in 8 KB steps to 8 KB; on the glasses the heap
refused 48 KB), with a 56-entry directory in the CFW RAM. ASSET_INFO reports
the size obtained. Kinds: 0 = 1bpp image (rows padded to
bytes), 1 = 4bpp image, 2 = UTF-8 string (up to 240 bytes).

- `08` ASSET_PUT `[id:u16][kind][flags][w:u16][h:u16][data...]`: `flags`
  bit 0 appends to the still-open asset with this id (images close when all
  their bytes have arrived; the newest asset is the only one that can grow);
  otherwise it creates the asset, replacing any entry with that id (bytes are
  reclaimed only when the replaced asset was the newest). Use STREAM records
  for images larger than one packet.
- `09` ASSET_DRAW `[id:u16][flags][x:u16][y:u16]`: blit a complete image
  asset; `flags` as BLIT (bit 1 flush, high nibble grey level for 1bpp).
- `0A` ASSET_TEXT `[id:u16][font][flags][x0][y0][x1][y1][max_lines]`: TEXT
  with a string asset.
- `0B` ASSET_CLEAR: forget every asset and reset the arena.
- `0C` ASSET_INFO: reply `[count][max][used:u24][arena_kib][allocated][arena>>8:u24]`.

All of these except INFO are valid stream records.

## `0D` SHAPES: `[flags][count][records...]`

Draws `count` 16-byte records straight into the frame:
`[type][flags][level][width][p0..p5 as int16 LE]`. Types: 0 line
(x0,y0,x1,y1), 1 rectangle (x0,y0,x1,y1), 2 circle (cx,cy,r), 3 triangle
(x0,y0,x1,y1,x2,y2). Record `flags` bit 0 fills rectangles, circles and
triangles; otherwise they are outlined with `width` pixels (1-16), which is
also the brush size for lines. `level` is the grey nibble. Coordinates are
signed and everything clips to 640x200. Payload `flags` bit 1 pushes the whole
frame afterwards. SHAPES is also a valid stream record.

## Retained objects (`20`-`24`)

Up to 96 object descriptors live in the second CFW RAM region (the stock
navigation map buffer at `0x2001bf24`); pixels come from assets, the font
engine or shape records, so showing a scene again costs one SHOW.

- `20` OBJ_PUT `[id:u16][version:u16][kind][flags][x:i16][y:i16][w:u16][h:u16][asset:u16][level][font][lines]`:
  define or replace an object. Kinds: 0 image (asset at x,y, grey `level`
  for 1bpp), 1 text (string asset in the box x,y,w,h with `font`/`lines`),
  2 rect (filled, `level`), 3 shapes (a string asset holding
  `[count][records]`). `flags` bit 0 = visible.
- `21` OBJ_SHOW `[flags][count][ids:u16...]`: compose the listed objects in
  order; bit 0 clears the frame first, bit 1 pushes it afterwards. An unknown
  id refuses the whole SHOW before drawing.
- `22` OBJ_HIDE `[flags]`: blank the frame (bit 1 pushes); objects stay.
- `23` OBJ_STATE: reply `[revision:u16][count][shown][id,version x up to 4]`.
- `24` OBJ_CONTROL `[op][id:u16]`: op 0 drops all objects, op 1 drops one.

- `25` OBJ_ANIM `[period_ms][count][entries: id:u16, tx:i16, ty:i16, frames, easing]`:
  glide up to 16 shown objects from their current position to (tx, ty) over
  `frames` frames, `period_ms` apart (10-250; 0 keeps the current period).
  Easing 0 linear, 1 smoothstep, 2 ease-out, 3 ease-in. The glasses render
  the frames themselves on the display thread: the stubbed stock refresh
  handler is redirected to the CFW tick, which re-composes the SHOW list,
  pushes the frame, sleeps the period and re-posts the stock refresh message
  until every glide has settled. A new ANIM replaces the glide set. Each
  frame pushes only the union of the moved objects' old and new boxes; a
  shapes object has no box and forces a whole-frame push.
- `26` ANIM_STATE: reply `[active][period_ms][ticks:u32][full_pushes:u32]`.

All but STATE are valid stream records.

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

## `12` PEEK: `[addr:u32][len]`

Diagnostic: reply body is `len` (at most 16) bytes read from `addr`. Used to
inspect candidate RAM before reclaiming it.

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
