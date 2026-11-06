# G1CFW/49: timed tweens and a 640x380 canvas

Revision 49 is stock 1.6.3 with two changes. It passes the offline suite
(90 tests, the ASan/UBSan host test and the shapes-suite replay) and has not
been flashed.

## Tweens finish on time

Up to /48 a tween advanced one step per rendered frame, and the display
thread slept the full period after each frame. A frame that took 30 ms to
compose and push therefore stretched a 33 ms-per-frame glide to about twice
its length. The G1CFW/19 notes measured 24 fps against a 30 ms request for a
40x40 square, and larger dirty areas were slower still.

From /49 progress is elapsed time over `frames x period`. This matches
g2flash's GLASSLYCFW/41. The clock is the stock `sys_clock_tick_get_32`
(`0x89e40`, 32768 Hz, guarded) shifted down to 1/1024 s. A tween counts as
started one period before its OBJ_ANIM ran, so the first tick shows the first
step and the last lands `frames - 1` periods later. The tick sleeps only what
is left of the period, with a 2 ms floor. A slow frame skips ahead; it no
longer slows the glide. On the host, ten 33 ms frames with 50 ms pushes end
in about 330 ms, not about 830 ms. ANIM_STATE adds `frame_ms`, the compose
and push time of the last frame.

Pivot values no longer take tween storage (they always applied at once). The
tween table therefore shrank from 64 to 60 bytes per entry. Message execution
and the display tick hold the stock LCD mutex. So a SHOW on the BLE thread and
a tween frame on the display thread no longer compose into the buffer at the
same time. A tick whose display-queue message was dropped (`k_msgq_put` with
no wait) is restarted by the next OBJ_ANIM after a second.

## 640x380 canvas

The stock heap has one 64,000-byte frame buffer and refuses even 24 KB more.
That leaves no room to store a taller frame, so the canvas is not stored.
Retained objects are re-composed for every push, at most 200 rows at a time.
The buffer holds one band, and the push sends it with the row origin moved
to the band. A whole-canvas push composes rows 200..379, then 0..199. A
tween frame composes only its dirty rows, so a band of up to 200 rows costs
one pass.

- Shapes, paths, rects and images were already clipped to a row range. They
  now draw into the band through a base pointer offset by the band.
- Text: the stock engine writes glyph rows through a row-pointer table, so
  the CFW passes its own 420-entry table (third RAM region, after the path
  workspace) with the box moved to engine row 0. Rows outside the band point
  at a scratch row. The engine clamps a glyph line's start row to 200 in
  `__copy_to_frame_buffer` (`0x80c0c`, `it ge; movs r6, #200`), and those
  two instructions become NOPs (raw patch, guarded). All callers are stock
  `gui_*` drawers. Their screens are stubbed or carved, and the stock UI never
  starts a line below row 199, so they are unaffected.
- Direct draws (BLIT, FILL, TEXT, SHAPES, ASSET_DRAW, ASSET_TEXT) still use
  canvas rows 0..199 of the buffer, the "window". After a band below row 200
  has been through the buffer, the next direct draw first re-composes the
  window from the objects. FLUSH takes canvas rows and re-composes the rows
  below 200. Direct pixels last until a band covering them is composed, and
  do not survive a panel wake.
- Panel wake and the 600 ms and 1.2 s retries now compose. The wear,
  work-mode and stock-event threads (stack sizes unknown) only flag the
  push, and the display thread does it. PANEL and VIEW still push on the BLE
  thread. A render error no longer leaves the LEDs off.
- INFO appends the canvas height (`u16`, 380). The buffer height byte stays 200.

The stock height gear puts the canvas top at `(8 - gear) * 35` (row 210 at
gear 2), so the bottom 110 rows of the canvas are off the panel until the
host moves it with VIEW. `g1flash.py view --height` spreads the gears over
the 100 rows a 380-row canvas can move: `(8 - gear) * 100 / 8`, giving row 75
at gear 2. **How much of the 480-row panel the optics show is not
measured.** The /44 report saw lit strips only about 5 rows past the 200-row
canvas at gear 2, so check by eye before laying out content near the canvas
edges.

## Code space

The blob reached 25.9 KB against the 24,856-byte carve. Cold handlers
(commands, stream, panel, input, IMU, microphone) are now built with
`__attribute__((minsize))`; composition, rasterizers, text and the push stay
at `-Os`. Size-optimized code loads rodata through PC-relative literals, so
`tools/build_blob.py` resolves `R_ARM_REL32` inside the blob. The blob is
23,028 bytes, with 1,828 left.
