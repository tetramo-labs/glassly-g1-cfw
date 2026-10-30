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
protocol revision (20 for this document) and the version is the stock base
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
its `off` callback, bypassing that policy; use these to draw while docked.
`mode` 4 calls the context's `suspend` callback (`panel_suspend`: SPI deep
power down) and 5 its `resume` callback (`panel_resume`: panel init plus
brightness; it refuses when bit 8 of the halfword at panel+0x4fc is clear),
`mode` 6 and 7 call the driver's suspend and resume directly (SPI deep
power down; `panel_init` plus brightness, rail untouched) and clear or set
the powered flag, for experiments. Reply body:
`[power_state][powered]`, where `power_state` is the stock byte (0 running,
1 suspended, 2 suspending, 3 resuming) and `powered` is the panel context
flag that FLUSH checks. While the glasses are docked the stock firmware
keeps the panel suspended, so a PANEL on is needed before anything becomes
visible there. Under the CFW with the auto panel enabled the stock suspend
is refused; with it disabled, `mode` 0 rebooted a temple on hardware, so
prefer 3 or 4 to switch the panel off.

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
bytes), 1 = 4bpp image, 2 = UTF-8 string (up to 240 bytes), 3 = path
(G1CFW/39).

- `08` ASSET_PUT `[id:u16][kind][flags][w:u16][h:u16][data...]`: `flags`
  bit 0 appends to the still-open asset with this id (images close when all
  their bytes have arrived; the newest asset is the only one that can grow);
  otherwise it creates the asset, replacing any entry with that id (bytes are
  reclaimed only when the replaced asset was the newest). Use STREAM records
  for images larger than one packet. A **path** (kind 3) is uploaded in one
  record (no append) as compiled commands in Q4 (1/16 pixel) signed int16
  coordinates: `0` M x y, `1` L x y, `2` Q cx cy x y, `3` C c0x c0y c1x c1y x y,
  `4` Z; every contour starts with M and closes with Z (`demos/vector-protocol.ts`
  in g2flash compiles the SVG `d` subset M L H V Q C S T Z, absolute and
  relative). `w` is the fill rule (0 nonzero, 1 even-odd), `h` 0. The glasses
  flatten the curves once (De Casteljau, at most 8 levels, half a pixel of
  flatness) and keep at most 128 edges per path, 16 bytes each behind a
  4-byte header; an unclosed contour, an unknown command, a curve that
  needs more edges or an append is `0D`, a full arena `0B`.
- `09` ASSET_DRAW `[id:u16][flags][x:u16][y:u16]`: blit a complete image
  asset; `flags` as BLIT (bit 1 flush, high nibble grey level for 1bpp).
- `0A` ASSET_TEXT `[id:u16][font][flags][x0][y0][x1][y1][max_lines]`: TEXT
  with a string asset.
- `0B` ASSET_CLEAR: forget every asset and reset the arena.
- `0C` ASSET_INFO: reply `[count][max][used:u24][arena_kib][allocated][arena>>8:u24]`.

All of these except INFO are valid stream records.

## `0D` SHAPES: `[flags][count][records...]` (20-byte records from G1CFW/38)

Draws `count` records straight into the frame with the vector rasterizer
ported from g2flash (`patches/shapes.c`). Every record is 20 bytes:
`[type][flags][color][width][p0..p7 as int16 LE]`. `color` is the grey
nibble, `width` the stroke in pixels for outlines and open strokes (0 is
drawn as 1), `flags` is reserved for objects (bit 0 visible) and ignored
here. Unused parameters are zero.

| type | shape | parameters |
| --- | --- | --- |
| 1 | LINE | x0 y0 x1 y1 (hairline at width 1, else a square-capped stroke) |
| 2 | RECT | x y w h r: stroked rounded rectangle, corner radius r |
| 3 | RECT_FILL | x y w h r |
| 4 | CIRCLE | cx cy r: ring of `width` pixels |
| 5 | CIRCLE_FILL | cx cy r |
| 6 | TRI | x0 y0 x1 y1 x2 y2, stroked |
| 7 | TRI_FILL | x0 y0 x1 y1 x2 y2 |
| 8 | QUAD | x0 y0 x1 y1 x2 y2 x3 y3, stroked |
| 9 | QUAD_FILL | x0 y0 x1 y1 x2 y2 x3 y3 |
| 10 | BEZIER2 | x0 y0 cx cy x1 y1: quadratic, stroked |
| 11 | BEZIER3 | x0 y0 c0x c0y c1x c1y x1 y1: cubic, stroked |
| 12 | ARC | cx cy r a0 a1: stroked arc in degrees, 0 = +x, clockwise |
| 13 | PIE | cx cy r a0 a1: filled sector |

Coordinates are signed canvas pixels, clamped to +/-1024 before any
arithmetic and clipped to 640x200. Circles are centred on the pixel
(cx, cy) and cover an odd 2r+1 pixel diameter (r = 1 is a 3x3 dot); corners
are quarter circles of radius r (clamped to half the shorter side); polygons,
wide lines, curves and sectors are scan-converted at pixel centres in 1/16
pixel units, so shared edges never gap or double-cover. A type of 0 or above
13 is `08`; a payload shorter than `count` records is `01`. Payload `flags`
bit 1 pushes the whole frame afterwards. SHAPES is also a valid stream
record. (G1CFW/13 to /37 used 16-byte records with types 0-3; those
builds answer PING with their revision, so a host can tell them apart.)

## Retained objects (`20`-`26`)

Up to 96 object descriptors live in the second CFW RAM region (the stock
navigation map buffer at `0x2001bf24`); pixels come from assets, the font
engine or shape records, so showing a scene again costs one SHOW.

- `20` OBJ_PUT `[id:u16][version:u16][kind][flags]` + body: define or
  replace an object. `flags` bit 0 = visible. Kinds 0-3 carry
  `[x:i16][y:i16][w:u16][h:u16][asset:u16][level][font][lines]` (19 bytes
  in all): 0 image (asset at x,y, grey `level` for 1bpp), 1 text (string
  asset in the box x,y,w,h with `font`/`lines`), 2 rect (filled, `level`),
  3 shapes (a string asset holding `[count][records]`, drawn in canvas
  coordinates; x,y,w,h unused). Kind 4 (G1CFW/38) SHAPE carries one 20-byte
  SHAPES record (26 bytes in all; the record's own flags byte is ignored):
  a single geometric primitive whose parameters, colour and stroke width can
  be animated. Kind 5 (G1CFW/39) PATH uses the kinds 0-3 body: a path asset
  drawn at `x`,`y` with `w` as a Q8 scale (256 = 1x, up to 2048) and grey
  `level`, filled by its own rule. Kinds 4 and 5 also carry a rotation
  (angle in Q4 degrees about a pivot in canvas pixels, animated through
  OBJ_ANIM values 10-12 below); a PUT resets it to 0. A PUT stops any
  animation of that object. A wrong kind, a record type outside 1-13 or a
  body shorter than the kind needs is `08`/`01`.
- `21` OBJ_SHOW `[flags][count][ids:u16...]`: compose the listed objects in
  order; bit 0 clears the frame first, bit 1 pushes it afterwards. An unknown
  id refuses the whole SHOW before drawing.
- `22` OBJ_HIDE `[flags]`: blank the frame (bit 1 pushes) and stop every
  animation; objects stay.
- `23` OBJ_STATE: reply `[revision:u16][count][shown][id,version x up to 4]`.
- `24` OBJ_CONTROL `[op][id:u16]`: op 0 drops all objects (and animations),
  op 1 drops one object (and its animation).
- `25` OBJ_ANIM `[period_ms][count][entries...]` (tween form from G1CFW/38):
  each entry is `[id:u16][mask:u16][frames][easing]` followed by one `i16`
  target value per set mask bit, low bit first. Mask bits 0-7 name the
  parameters of a kind-4 shape (p0..p7; only the type's geometry bits are
  valid: LINE `0x0F`, RECT/RECT_FILL/ARC/PIE `0x1F`, CIRCLE/CIRCLE_FILL
  `0x07`, TRI/TRI_FILL/BEZIER2 `0x3F`, QUAD/QUAD_FILL/BEZIER3 `0xFF`) or, for
  kinds 0-2, bit 0 x, bit 1 y, bit 2 w, bit 3 h (w/h only for text and
  rect) or, for kind 5, bit 0 x, bit 1 y, bit 2 the Q8 scale. Bit 8
  (`0x100`) is the colour nibble (kinds 4 and 5) or `level` (kinds 0 and 2;
  the stock text drawer has no level), bit 9 (`0x200`) the stroke width
  (kind 4 only), bits 10-12 (`0x400`, `0x800`, `0x1000`) the rotation angle
  in Q4 degrees (16 per degree, so 1440 is a quarter turn; unwrapped, so
  5760 is a full turn) and the pivot x and y in canvas pixels (kinds 4 and
  5 only). A rotated primitive is drawn as a contour (rounded rectangles,
  rings and arcs become polylines, strokes stay `width` wide) and pushes the
  whole frame. Each entry interpolates its object from its current
  values (mid-flight if it was already animating) to the targets over
  `frames` frames, `period_ms` apart (10-250; 0 keeps the current period);
  `frames` 0 applies the targets at once. Entries merge into the running
  set by object id, so an ANIM for one object never disturbs another's
  glide; up to 16 objects animate at a time (`0B` when the set is full).
  Easing 0 linear, 1 smoothstep, 2 ease-out, 3 ease-in. An unknown id is
  `0E`, a mask bit the kind cannot animate is `08`, a truncated entry list
  `01`. The glasses render the frames themselves on the display thread: the
  stubbed stock refresh handler is redirected to the CFW tick, which
  re-composes the SHOW list, pushes the frame, sleeps the period and
  re-posts the stock refresh message until every tween has settled. Each
  frame pushes only the union of the animated objects' old and new boxes
  (kind-4 shapes have a type-aware box); a kind-3 shapes object has no box
  and forces a whole-frame push. (G1CFW/17 to /37 took fixed 8-byte glide
  entries `id, tx, ty, frames, easing`.)
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
message; `records done` says how many ran. A flush that meets the panel off
does not stop it (G1CFW/32): the pixels are in the frame buffer and go out
with the whole frame when the panel comes on, every record runs, and the
reply carries status `04` so the host knows nothing is visible yet.

## `11` STATS

Reply body: `[messages:u16][errors:u16][stock_swallowed:u16][last_status]
[message_active][message_bytes:u16][message_max>>8][ram>>8]`.

## `12` PEEK: `[addr:u32][len]`

Diagnostic: reply body is `len` (at most 16) bytes read from `addr`. Used to
inspect candidate RAM before reclaiming it.

## `14` VIEW: `[op][dx:i16][dy:i16]` (G1CFW/23 and later; dx as content shift from G1CFW/26)

Where the 640x200 frame lands on the panel. The stock UI turns the phone's
height gear into the panel row origin ((8 - gear) * 35 rows; both temples
boot at 210, gear 2) and the depth gear into an opposite horizontal shift of
the two lenses' content (gears 1..9 shift 22, 8, 0, -4, -8, -10, -12, -14,
-16 columns, negated on the master temple). `dy` is added to the stock row
origin at every push. `dx` cannot move the column origin: the panel's
column space is exactly 640 wide, so a shifted origin clips one edge and
wraps the other (seen on hardware with G1CFW/23). Instead each push shifts
the affected frame rows by `dx` in place, blanks the strip the canvas no
longer covers, pushes, and restores the rows, so drawing stays in canvas
coordinates and content near the far edge is cropped. `dx` must be even
and within +/-24 (the stock table tops out at 22); `dy` within +/-480.
`op` bit 0 = query only. Reply body `[dx:i16][dy:i16][stock_col:i16]
[stock_row:i16][height_gear][depth_gear]`. From G1CFW/38 setting the view
while the panel is powered also blanks every panel row outside the new
canvas and pushes the whole frame (op bit 1, the old explicit push, is
accepted and redundant), so a connecting host that places the canvas wipes
whatever the stock screens left on the panel before it. `g1flash.py view
--height H --depth D` map the stock gears per temple, `--dx/--dy` set the
offset directly.

## IMU streaming (`33`-`34`, G1CFW/23 and later)

The IMU fusion thread runs on the master (right) temple and keeps three
attitude words (hundredths of a degree: roll-like, drifting yaw, pitch).

- `33` IMU_CONTROL `[period_ms][count:u16]`: stream `count` samples (`ffff`
  = until stopped, 0 = stop) every `period_ms` (10-250). Samples are sent
  from the display-thread tick that also drives animation; while a glide
  runs, its period sets the pace. Reply `[active][period_ms][remaining:u16]
  [sent:u32]`.
- `34` IMU_SAMPLE: unsolicited `F0 34 00 [seq:u16][word0:i32][word1:i32]
  [pitch:i32]`.

## `40` MIC_CONTROL: `[op]` (G1CFW/25 and later)

`op` 1 starts a capture session, 0 stops it, 2 queries. Start does what the
phone's stock open-mic command (`0E 01`) does without its screen-state
checks: it routes encoded chunks to BLE rather than the quick-note flash
recorder, sets the capture mode to "stream" and asks the main thread for a
PDM session. The session reads 16 kHz mono blocks, LC3-encodes them and
the BLE dispatcher sends them to the connected central as stock
`[F1][seq][200-byte chunk]` notifications. Stop sets the mode byte that
makes the session loop exit after its next block. Nothing stops the stream
on disconnect; the host must send `op` 0 (or the glasses must reboot).
Reply body `[recording][starts:u16][stops:u16][mode][armed]` (`mode` 0
stream, 1 stopped, 2 capture without sending; `armed` non-zero while the
main thread owns a session). `g1flash.py mic record --seconds N --out
file.lc3` starts, captures, stops and writes the raw concatenated chunks;
`tools/lc3_decode.py file.lc3` turns the capture into a WAV through liblc3
(`brew install liblc3`): each 200-byte chunk is ten 10 ms LC3 frames of
20 bytes (16 kbit/s) at 16 kHz, so one chunk arrives every 100 ms.

## Input events (`30`-`32`, G1CFW/21 and later)

Two stock paths are redirected into the CFW so the host can observe what
the glasses sense:

- **Touch gestures** (G1CFW/21). Each temple's capacitive touch chip wakes
  the stock `touch_key_thread`, which classifies the press history into a
  gesture code and hands it to the stock key-event thread through a small
  helper; that helper is now `cfw_touch_event`. Codes: 1 single tap,
  2 double, 3 triple, 4 long-press start (stock turns the mic on), 5
  long-press end, 6 long-press timeout. Five, six, ten, eleven and fifteen
  taps act inside the stock thread (reboot, force bind, user data reset,
  ESB band) and never reach the helper, so they are not reported.
- **Stock events** (G1CFW/22). Everything the stock firmware reports to the
  phone as an `F5` notification passes through one poster function, now
  `cfw_stock_event`: 2 head up, 3 head down (IMU fusion thread); 6 worn,
  7 taken off outside the case, 8 placed in the open case, 9 charging
  state, `0a` battery percent, `0b` case closed, `0e` case charging state,
  `0f` case battery (work-mode checker); `11` sent once per connection
  after the bind check; plus key and dashboard events the stock UI emits
  (`13`-`16`, `18`, `1e`, `20`, 0).
  The CFW re-implements the poster (direct `[F5][code][CB]` packet when the
  link is ready, otherwise the BLE context's event semaphore), so the stock
  phone path is unchanged unless swallowed.

- `30` INPUT_CONTROL `[flags]`: bit 0 report gestures, bit 1 keep gestures
  from the stock handler (no dashboard, mic or temple sync), bit 2 report
  stock events, bit 3 keep stock events from the phone path, bit 4 leave
  panel power to the host. All off at boot, so a bare host sees no
  unsolicited traffic and the panel follows wear. Since G1CFW/31 the body
  may be `[flags][wear_on][wear_off]`: the CFW's own wear thresholds on the
  radio-link word (worn at or above `wear_on`, not worn at or below
  `wear_off`, defaults 56 and 53, `wear_on` must exceed `wear_off`). Reply
  body `[flags][wear_on][wear_off]`; an unknown bit or a bad pair is
  `bad-record`.
- `31` INPUT_STATE: reply `[flags][last_gesture][gestures:u16]
  [gestures_passed:u16][last_event][events:u16][events_sent:u16]
  [panel_autos][work_mode][last_mode][mode_changes][checks][suspends_skipped]`
  (since G1CFW/28-29: the stock work mode now, the last mode asked of
  `change_work_mode`, and the low bytes of three counters: mode changes,
  200 ms checker passes, stock display suspends refused).

**Panel follows wear (G1CFW/27, reworked in /28).** With the stock refresh
pipeline stubbed nothing asks for the panel when the glasses are put on, so
unless bit 4 is set the CFW switches it: on through the panel's resume
callback, falling back to the on callback plus a brightness write when
resume refuses (it does on these units), then a push of the whole frame
buffer, repeated 600 ms and 1.2 s later on the 200 ms ticks (G1CFW/35).
The buffer survives the power-up: the whole-frame clear inside the stock
`panel_init` and the panel's clear callback are stubbed (G1CFW/36), so the
panel comes up showing what the host drew last, deferred draws included,
and the CFW blanks the 280 panel memory rows outside the canvas itself
(G1CFW/37), which that stock routine used to do.
Off goes through the suspend callback, falling back to the off callback. Two sources
feed it. The stock events `worn` (6), `taken off` (7), `in case open` (8)
and `case closed` (`0b`) only flow while a central is connected (the stock
wear state machine resets when nothing is). The stock work-mode switch
(`change_work_mode`, detoured at its entry) runs whether or not one is:
modes 2 `normal-run` and 3 `normal-standby` mean worn and switch on, 4
`normal-low-power` (taken off), 5 and 6 (charging in the case) and 8 (deep
sleep) switch off; 0, 1, 7 and 9 are left alone. G1CFW/29 adds the two
pieces that make this hold: the stock work-mode checker, which runs every
200 ms, reconciles panel power with the current mode on each pass (the
stock display-off path switches the panel without a mode change), and the
stock system suspend is refused outright while the CFW manages the panel,
so nothing blanks a worn pair but the host. G1CFW/30 adds the piece that
makes the wear sensor itself report: the sensor is the radio link between
the temples (the network core reports its ESB statistics over IPC about
every six seconds; the checker classifies the fourth word, `>= 0x33` worn,
`< 0x28` not worn), and the IPC handler overwrites that word with `0x46`
(worn) whenever the wear-detection user setting is off, so a pair with the
setting off never leaves normal standby. The 200 ms pass turns the setting
on while the CFW manages the panel. `g1flash.py touch state` reads the
live word and the setting alongside INPUT_STATE through PEEK. The stock
thresholds leave a pair lying unfolded on a table (word 47-50 measured
here; on a head 62-74, folded in the case 25) in a dead band for ever, so
from G1CFW/31 the CFW classifies the word itself while the stock mode says
worn: panel on at or above `wear_on`, off at or below `wear_off`, unchanged
in between, and a word of 0 (no statistics yet after boot) trusts the
mode. `g1flash.py touch control --wear-on N --wear-off M` sets the pair.
With bit 4 set everything stands down and the stock suspend works again.
PANEL (`05`) still works for manual control.
- `32` INPUT_EVENT: unsolicited `F0 32 00 [class][code][count:u16]`, class 0
  gesture, class 1 stock event, from the temple that produced it. The host
  tool (`g1flash.py touch watch`) sets bits 0 and 2, prints events for a
  while and clears the flags on exit.

The flags are per temple and live in RAM until reboot.

## Link policy (G1CFW/20 and later)

The stock firmware accepts only the bonded phone's address while out of the
case and drops any unbonded central 30 s after it connects. G1CFW/20 patches
both checks out, so any BLE central can connect at any time and stay
connected. There is no authentication on the private protocol; this is a
development firmware.

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
