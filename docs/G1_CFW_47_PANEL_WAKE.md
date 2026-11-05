# G1CFW/47: clear the panel before showing it

Revision 47 stays on stock 1.6.3 and fixes the firmware sequence responsible
for the full-panel flash followed by a visible row-by-row clear on wear wake.
The implementation passes offline tests; visual behavior needs confirmation
on glasses.

## Cause and change

Previously the CFW stubbed `0x80d76`, the full-panel clear called by stock
`panel_init` at `0x80e50`. This preserved the 640×200 host framebuffer, but
also skipped clearing the physical 640×480 panel cache. Stock subsequently
sent LED enable (`A3` at `0x80eae`) before the CFW cleared its margins and
restored the saved image. That exposed uncleared panel memory. The margin
clear also used the viewport column offset, leaving physical edge coverage
dependent on the displaced canvas.

The new `cfw_panel_init_clear` replaces that stub. It sends LED disable
(`A9`), clears the entire physical cache at column zero, synchronizes the
pixel latch (`97`), and waits 1 ms before returning to stock initialization.
It works before the stock powered flag is set and never touches the saved
framebuffer. Stock can only enable a black panel afterward.

The post-wake redraw and VIEW changes now hold the LCD mutex through LED
disable, physical clear, retained-frame drawing, latch synchronization and
LED enable. A failed framebuffer transfer leaves output disabled for the
next retry. A completely off-panel viewport still latches black before
enabling output. The existing 600 ms / 1.2 s recovery redraws are retained;
they use the same hidden clear/redraw sequence.

The early init hook has two stock callers: `panel_init` and the screen-exit
wrapper at `0x80dbc`. The latter remains stubbed, so stock screen switches
cannot invoke the new clear while CFW owns the display.

## Implementation evidence

The exact 1.6.3 binary supplies three additional guarded stock helpers:

| Entry | Purpose |
| --- | --- |
| `0x80f08` | SPI packet write, preserving chip-select framing |
| `0x80ece` | Single panel command |
| `0x80ec4` | Millisecond busy wait |

The panel initialization instruction sequence is also guarded in
`cfw/targets/g1-1.6.3.json`. The replacement hook and its position before LED
enable are checked against the signed stock image.

Packets contain a five-byte `02/address/FF` header, eight rows of zero pixels
(2,560 bytes) and the trailing zero byte, using physical addresses `(0,y)`
for `y=0,8,...,472`. Sixty transactions clear every physical row, including
top, bottom and side strips. They use the existing third RAM region under
the LCD mutex, which also protects its depth-shift scratch use. The old
320-byte parked framebuffer row is no longer needed, leaving 328 bytes free
in RAM region 2. No extra heap allocation or large stack buffer is introduced.

The command framing, full-cache clearing and 1 ms sync guard agree with
[scouter_kit's panel driver at the reviewed commit](https://github.com/mafik/scouter_kit/blob/8cb6334b54ce48ae670c5be7fe6da9e5b7f409e0/g1_firmware/firmware/src/panel.c).
The stock disassembly establishes the callable addresses and initialization
order for this patch; no foreign firmware offsets are reused.

## Artifact and checks

The signed application is still 970,085 bytes; its injected blob plus helper
uses 24,016 bytes, leaving 840 bytes in the carved flash region.

- File SHA-256: `dbda933f49143f7af0130b1d904c9515dcdda5263f802b83f09b43bd5f602845`
- MCUboot image hash: `ca13ce789feba048a51e2d0e05b4d768da40993cf2c18b962a9a777a99c69944`
- Previous G1CFW/46 remains an accepted upgrade source.
- Both Glassly platform bundles and catalog pins are updated to revision 47.

All 90 firmware tests pass, including ASan/UBSan execution of the C handler.
The host panel model starts with a white cache and latch, verifies all 480
rows are cleared while output is disabled, enforces sync delays, rejects
enable before the latch matches the cache, and checks preservation of the
entire retained frame. Tests cover positive/negative column offsets,
completely clipped views, failed redraws, scratch canaries and the binary
init-hook ordering. These checks validate sequencing and memory coverage;
they do not substitute for observing the wake cycle on both temples.

Glassly also passes its Android SDK build and 63 G1 unit tests, plus the iOS
SDK build and 61 G1 simulator tests. The built AAR and iOS resource bundle
contain the exact signed revision 47 image and match their catalog pins.
