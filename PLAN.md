# G1 custom firmware plan

Updated: 2026-10-28

## Goal and current scope

Backport the smallest useful part of `~/dev/g2flash` to Even Realities G1 glasses,
cutting scope as needed to establish a working custom-firmware install on both
temples. The flash/boot/readback milestone is complete, code injection works, and a first bounded display protocol is on the glasses.

The first milestone is a signed stock-based application with an observable
`G1CFW/1` diagnostic marker. This is a flash/boot/readback milestone, not yet a
port of the G2 graphics runtime or its features.

## Completed

- Cloned `JohnRThomas/even_realities_decomp` into `~/dev/even-g1-decomp`.
- Inspected `~/dev/g2flash` and reused its pinned-input, expected-byte patching,
  deterministic-output approach. G2 offsets and OTA protocol are not compatible.
- Consulted Reverse Engineer Anything at `/tmp/g1-rea` as a reference; its
  analysis engine was not required for the current marker patch.
- Identified and verified the public stock package labelled 1.5.6. Its actual
  diagnostic version is **1.5.60**, build **2025-03-21 11:38:43**.
- Verified stock application and network image RSA-PSS signatures against the
  public MCUboot development key. Device acceptance remains unverified.
- Created a reproducible application-only build with a same-length diagnostic
  string patch, updated digest, and valid signature. No executable code changes.
- Implemented offline image validation and install/restore plans, explicit
  device/side selection, diagnostic checks, local DFU entry, SMP staging,
  activation, confirmation, upload resumption, timeouts, and persistent logs.
- Preserved the exact stock application and original archive for restore.
- Passed 30 offline tests, including simulated install, confirmation, restore,
  interrupted transfers, and refusal of incorrect images/devices.
- Built the CFW, validated its flash plan, and flashed it to both temples with a
  demonstrated stock restore in between (see below).

## Reproduce the current build

```sh
cd ~/dev/even-g1-decomp
./build_cfw.sh
G1_STOCK_ZIP=build/g1-mvp/stock-firmware.zip .venv/bin/python -m unittest discover -s tests -v
.venv/bin/python g1flash.py plan --firmware build/g1-mvp/app_update.bin
.venv/bin/python g1flash.py plan --firmware build/g1-mvp/stock-app_update.bin
```

Artifacts under `build/g1-mvp/`:

- `app_update.bin`: signed marker CFW, 976,509 bytes.
- `g1-cfw-marker.zip`: application-only update package.
- `stock-app_update.bin`: exact stock application for restore.
- `stock-firmware.zip`: original vendor package.
- `flash-plans.json` and `build-report.json`: identities and validation results.

Expected CFW file SHA-256:
`8719c710f78f9c4967f83a47e4617f9f9b3b10b69da0100c53a55616737f3177`.

The signed-region hash used by SMP differs from the whole-file hash. The tools
validate both; use their generated plans rather than substituting one for the other.

## Done: validated on one temple, then the pair (2026-10-28)

Both temples (`Even G1_26_L_C98EE7`, `Even G1_26_R_48D3D5`) were on the exact
pinned base (`ver 1.5.60`, app build `2025-03-21 11:38:43`, stock signed-region
hash active and confirmed, battery 100%). Observed on hardware, in order:

1. Left: staged, activated, read back `G1CFW/1`; CFW hash active and confirmed.
2. Left: restored exact stock the same way; stock diagnostic and hash verified.
3. Left: CFW reinstalled. Right: CFW installed. Both verified and confirmed.

Facts learned, recorded in [docs/G1_CFW_MVP.md](docs/G1_CFW_MVP.md):

- SMP is exposed in normal mode; no DFU-entry command was needed or sent.
- Stock firmware drops an unbonded central 30 s after connecting. Uploads
  resume by hash, so `stage` is rerun until it reports `uploaded`. Bonding was
  deliberately avoided because it would unpair the phone.
- `stage --chunk-bytes 480` is accepted (MTU 498) and finishes in about seven
  30 s windows. `g1flash.py` now fails fast on disconnect.
- The bootloader is overwrite-only with auto-confirm: no test/revert state and
  no on-device copy of the previous image. Restore is OTA only, and it works.
- A rebooted temple takes about two minutes to re-advertise.

Still unverified from the host: display, phone pairing, and charging on the CFW
(expected unchanged, since only a 15-byte string differs from stock).

## Done: code injection and the first display port (2026-10-28)

g2flash's approach now works on the G1 (see the code-injection section of
[docs/G1_CFW_MVP.md](docs/G1_CFW_MVP.md) and
[docs/G1_CFW_PROTOCOL.md](docs/G1_CFW_PROTOCOL.md)):

- `tools/build_blob.py` compiles `cfw/src/g1cfw.c` with Apple clang to a
  position-independent Thumb-2 blob; `tools/gen_profile.py` appends it to the
  signed image, patches one `bl` in the BLE receive entry, re-signs, and writes
  a profile that `./build_cfw.sh` replays without a compiler.
- Flash budget: slot 0 ends at `0xfc000`, so about 2.4 KB is appendable. The
  G1CFW/5 blob uses 1168 bytes. Growing beyond that means carving factory-test
  code (about 4 KB of candidates identified) rather than appending.
- Private command `0xF0` with PING, BLIT (1bpp/4bpp rects), FILL, FLUSH,
  INFO, and PANEL. Verified on the left temple: the hook passes stock traffic
  through, replies work, the frame buffer and `_reflash_fb_data_to_lcd` path
  push a full frame, and the panel can be powered while docked through the
  context's own `panel_on` callback (the stock resume path refuses while
  docked). `tools/g1dis.py` found every 1.5.60 address; the 1.6.3 decomp is
  reference only.
- Profiles: `g1-1.5.6-marker`, `cfw2-ping`, `cfw3-display`, `cfw4-panel`, and
  the current `g1-1.5.6-cfw` (G1CFW/5). Each lists the images it may be
  installed over, so upgrades and the stock restore work from any of them.
- Tests: 32, including a sanitizer-backed host build of the injected C.

## Scope decisions (2026-10-28)

- Stock behaviour does not need to be preserved in the CFW. Stock subsystems
  the CFW does not use may be stubbed and their flash reused for injected code.
  What must keep working: boot, Zephyr and the BLE stack, the Nordic UART and
  SMP services (OTA restore depends on them), the panel driver and flush path,
  charging and power management, and temple-to-temple sync.
- Out of scope for g2flash parity: anything involving the R1 ring, the
  microphone volume gate, the BLE keyboard bridge, and the ambient light sensor.

## Next: g2flash parity under the new scope

Flash is no longer the blocker: G1CFW/7 lives inside the stubbed
`DashBoard_Reflash` body (15.8 KB region, 14.5 KB free) and the UI task block
offers about 38 KB more once its 80 entry points are stubbed. In order:

1. **Own the display.** Done in G1CFW/8: `display_reflash_handler` and the
   whole-frame clear are stubbed, so the stock display thread never writes
   the frame again. Still open: run CFW flushes on the display thread instead
   of the BLE worker (hook the stubbed refresh handler).
2. **CFW context in RAM.** Done in G1CFW/9: a magic-guarded context plus a
   4 KB message buffer live in the reclaimed BMP staging RAM at `0x200174e4`
   (5,120 bytes); stock `0x15`/`0x16`/`0x20` are swallowed. The stock upload
   queue is heap-allocated (`k_msgq_alloc_init(201, 30)` at `0x24c6c`), so it
   is not a static region to reclaim; larger CFW buffers (asset store) should
   come from the stock heap through `malloc` at `0x7a15c`, the call that
   allocates the 64 KB frame buffer, taken once at first use like g2flash's
   lease-scoped allocations.
3. **Message transport.** Done in G1CFW/9: STREAM packets with sequence
   numbers, CRC-16 per message, one reply per message, batched records
   (fill, blits, flush). Pending: hardware throughput numbers, and a resend
   policy in the host tool when a message is refused.
4. **Compression and deltas.** PackBits RLE done in G1CFW/9 (test pattern
   16,000 to 5,077 bytes); rect-to-rect COPY done in G1CFW/11. Dirty-rect
   updates are already expressible as BLIT records. Next: zlib inflate once a
   30 KB+ region is carved.
5. **Text.** Done in G1CFW/10 on the stock font engine (fonts 0-2, left
   aligned, wrapping, `\n`). Not yet: alignment, grey levels (the stock glyphs
   are on/off only), uploaded fonts.
6. **Assets.** Done in G1CFW/12 for images (1bpp/4bpp) and strings: a 48 KB
   heap arena with a 56-entry directory, PUT/DRAW/TEXT/CLEAR/INFO. Not yet:
   uploaded fonts, cached draw commands, LRU eviction (CLEAR only).
7. **Vector shapes.** G1CFW/13 draws lines (brush width), rectangles,
   circles/rings and triangles, filled or outlined, clipped and signed, as
   16-byte records. Not yet: rounded rectangles, arcs/sectors, beziers, image
   and text records inside a shape list. The **retained object cache**
   (G1CFW/16: PUT/SHOW/HIDE/STATE/CONTROL, 96 descriptors in reclaimed RAM at
   `0x2001bf24`) and **animation** (G1CFW/17: eased glides stepped on the
   display thread via the hooked refresh handler, `k_msleep` and the stock
   refresh post) are in, with dirty-row composition and dirty-rect pushes
   (G1CFW/19: 49 fps at a 10 ms period, 83+ at 1 ms). The agreed target is
   30 fps, so the default period is 20 ms. Not yet: tween of size/level,
   journal deltas, request-id replay protection.
8. **Inputs and sensors in scope:** touchpad gestures (G1CFW/21: every
   gesture code passes through the CFW; the host can watch them and keep
   them from the stock handler) and the stock event stream (G1CFW/22: head
   up/down, worn/taken off, case open/closed and the other `F5` events pass
   through the CFW; head events and the IMU live on the master, right,
   temple). Raw attitude (roll, yaw, pitch in hundredths of a degree) is
   readable live through PEEK (`g1flash.py imu`) and, from G1CFW/23,
   streamed by the glasses (`imu --stream`). G1CFW/23 also adds VIEW: the
   stock height/depth gears as a CFW offset on the panel origins
   (`g1flash.py view`). G1CFW/25 adds MIC_CONTROL: arm/stop the stock
   PDM capture session; the LC3 stream arrives as stock `F1` notifications
   (`g1flash.py mic record`). Next: wakeword and screen-wake takeover, raw
   PCM or per-microphone selection if the stock audio thread allows it.
   Each needs its own trace through the 1.5.60 binary. Out of scope: the
   R1, the volume gate, the BLE keyboard bridge, the light sensor, and a
   buzzer or compass (the G1 has neither). The 1.6.3 decompilation at
   https://github.com/JohnRThomas/even_realities_decomp/tree/repro163 is a
   reference for function names and control flow; addresses come from the
   1.5.60 binary.
9. **Phone path.** G1CFW/20 removes the stock central policy (any central
   may connect and stay connected; no pairing). Verified: the Mac pinged the
   left temple while the glasses were worn, out of the case, which unblocks
   the sensor work. A phone client for
   `0xF0` is still wanted for daily use.

## Known limits and handoff

- Bootloader key acceptance, the SMP transport, and activation policy are now
  verified on this pair. Other firmware versions still need their own profile.
- The restore script requires a functioning application with SMP reachable.
  Wired recovery has not been established, and the bootloader offers no
  automatic rollback (overwrite-only).
- Do not use the upstream development-kit `west flash --recover` instruction on
  the glasses; it erases flash.
- Current implementation is in `build_cfw.py`, `g1flash.py`, `cfw/` (including
  the injected C under `cfw/src/`), `tools/`, and `profiles/`; tests are in
  `tests/`. Regenerating a profile needs clang and the public MCUboot
  development key in `build/keys/`. Build outputs and the Python environment
  are local artifacts. Work has not been committed or pushed.
- Detailed commands, protocol evidence, image hashes, and recovery caveats are
  recorded in [docs/G1_CFW_MVP.md](docs/G1_CFW_MVP.md).
