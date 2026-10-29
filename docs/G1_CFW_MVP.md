# G1 CFW MVP

Status: **flashed and read back on hardware (see the session section below)**.

The first milestone is a stock-firmware patch that answers the existing Nordic
UART `#t` diagnostic command with `G1CFW/1 net bt:` instead of `net build time:`.
It proves the patch/sign/package/update/boot/readback path before introducing
new executable code. It is not yet the G2 graphics runtime.

## Scope

Reuse the `~/dev/g2flash` approach: exact stock SHA-256, expected old bytes,
committed replacement bytes, exact output SHA-256, and validation before upload.
The implementation here is independent, uses G1 MCUboot images and Nordic
MCUmgr SMP, and does not import G2 code or copy its offsets/protocol.

Cut from this milestone: retained objects, vector rendering, compression,
animation, custom command streams, new display ownership, sensor streaming,
phone integration, and rebuilding the decompiled application.

The marker is a same-length 15-byte replacement in read-only data. No executable
instruction, reset vector, image header, memory layout, settings, radio image,
bootloader, BLE name, or application version number changes. The SHA256 and
RSA-PSS TLVs are replaced to authenticate the modified image. The output ZIP
contains only `app_update.bin` and an application-only manifest.

## Reproduce

Python 3.11+:

```sh
./build_cfw.sh
```

An offline build accepts the exact original CDN archive:

```sh
.venv/bin/python build_cfw.py --stock /path/to/g1-stock-1.5.6.zip
G1_STOCK_ZIP=/path/to/g1-stock-1.5.6.zip .venv/bin/python -m unittest discover -s tests -v
```

Outputs are under `build/g1-mvp/`: signed `app_update.bin`,
`g1-cfw-marker.zip`, `stock-app_update.bin`, the original `stock-firmware.zip`,
`flash-plans.json`, and `build-report.json`. The original stock application is
kept for the restore path; its signature is not changed. Subsequent builds reuse
the cached stock ZIP and verify its hash again. The committed profile includes the
RSA signature, so normal builds do not need a private key and reproduce exactly.
The stock binary is downloaded locally, not committed.

## Offline flash plans

Neither command below imports Bluetooth or contacts a device:

```sh
.venv/bin/python g1flash.py plan --firmware build/g1-mvp/app_update.bin
.venv/bin/python g1flash.py plan --firmware build/g1-mvp/stock-app_update.bin
```

The selected binary determines the direction. The CFW binary requires the exact
stock active hash; the stock binary requires the exact CFW active hash. Both
paths validate the complete file hash and RSA signature before starting any
Bluetooth worker. There is no arbitrary-firmware or force mode.

Every hardware command writes a new JSONL journal under `build/logs/`, including
the selected device, plan, observed state, and activation/reset checkpoints.
`--log PATH` chooses a new file explicitly and refuses to overwrite an existing
log. A separate worker process enforces a 90-second deadline (30 minutes for
staging), even if native Bluetooth initialization blocks Python's event loop.
A timeout after a reset request is ambiguous: inspect the log and reconnect to
verify state rather than assuming the flash failed or repeating the reset.

## On-device workflow

Disconnect the phone's companion app, charge the glasses, and identify both
temples. Run one temple through boot verification before touching the other.
These commands require the target to advertise as `Even G1..._L_XXXXXX` or
`Even G1..._R_XXXXXX`. No automatic first-device selection is provided.

```sh
.venv/bin/python g1flash.py scan
.venv/bin/python g1flash.py probe --device LEFT_UUID --side L
.venv/bin/python g1flash.py probe --device RIGHT_UUID --side R
```

`probe` sends only the stock `#t` diagnostic request and records the raw reply.
The template's strings identify this particular stock build as
`2025-03-21 11:38:43`, `ver 1.5.60`. Although the public firmware link is labelled
**1.5.6**, the actual version constants are **1, 5, 60**. Never choose a firmware
profile based only on a marketing/version label.

SMP was observed in normal mode on 1.5.60, so DFU entry is not needed. The
explicit local-temple DFU command remains available but untested:

```sh
.venv/bin/python g1flash.py enter-dfu --device LEFT_UUID --side L
.venv/bin/python g1flash.py scan
.venv/bin/python g1flash.py state --device LEFT_DFU_UUID --side L
```

DFU entry is inferred from decompiled `ble_process_req_dispatch` and
`master_process_put_req`: `4a 01` requests the local reboot; bit 1 would also
forward to the other temple. This has **not** been tested on hardware. A changed
address/name must be inspected; the tool deliberately refuses unlabelled `DFU`
devices. A missing characteristic, MTU, state/hash mismatch, or different name is
a reason to gather evidence and adapt the transport, not bypass its checks.

```sh
# Rerun until it reports "uploaded"; each run resumes after the 30 s drop.
.venv/bin/python g1flash.py stage --scan-seconds 30 --chunk-bytes 480 \
  --device LEFT_UUID --side L --firmware build/g1-mvp/app_update.bin
.venv/bin/python g1flash.py activate --scan-seconds 30 --device LEFT_UUID --side L \
  --firmware build/g1-mvp/app_update.bin
# Reconnect after reboot, with the current normal-mode UUID:
.venv/bin/python g1flash.py verify --device LEFT_UUID --side L
```

`stage` checks the active application hash and confirmed flag, uploads image 0,
and checks the inactive application's MCUboot digest and bootable flag. It does not mark it
pending or reset. `activate` repeats the checks, requests a test boot, verifies
the pending state, then requests reset. A reset ACK is not proof of boot success.
`verify` must observe the CFW diagnostic marker on the selected temple.
Use `state` to observe the new active image and its `confirmed` flag. If SMP
remains available after the test boot, explicitly confirm the expected image:

```sh
.venv/bin/python g1flash.py confirm --device LEFT_UUID --side L \
  --firmware build/g1-mvp/app_update.bin
```

`confirm` refuses any other active image and checks the response's confirmed
flag. It is a no-op if already confirmed. If SMP is unavailable in normal mode,
do **not** assume entering DFU again preserves an unconfirmed image: that
transition reboots. Establish stock auto-confirm behavior first. The explicit
`activate --permanent` option selects a permanent image before reset when that
is the intended mode; it is never selected automatically. No automatic rollback
is promised for either activation mode.

Activation may resume an already pending **identical** image with the same
permanence setting. If the target is already active, it reports that state
without selecting or resetting again. Other pending images are refused.

Verify display, pairing, charging, image confirmation, and another DFU connection
before repeating on R.

The active image must match the profile's **signed-region hash** exactly. A
different installed firmware is refused; there is no downgrade/force switch.
For a newer pair, first acquire its matching stock package and port the marker
profile against that binary. The repo's 1.6.3 decompilation offsets are not valid
for the 1.5.6 package.

Do not assume test-boot rollback works: the repository's sample uses
`CONFIG_BOOT_UPGRADE_ONLY=y`, but does not contain a verified production
bootloader dump. Nor is wired recovery established. Do not run the upstream
README's `west flash --recover` against the glasses: that instruction is for a
development kit and erases flash.

## Restore the exact stock application

This is an OTA restore path for a functioning CFW installation, not a wired
unbrick facility. It was exercised successfully on the left temple on 2026-10-28. It requires the same known target and a confirmed current
image. Enter DFU as above if needed, then:

```sh
# Rerun until it reports "uploaded"; each run resumes after the 30 s drop.
.venv/bin/python g1flash.py stage --scan-seconds 30 --chunk-bytes 480 \
  --device LEFT_UUID --side L --firmware build/g1-mvp/stock-app_update.bin
.venv/bin/python g1flash.py activate --scan-seconds 30 --device LEFT_UUID --side L \
  --firmware build/g1-mvp/stock-app_update.bin
# Wait about two minutes for the temple to re-advertise, then:
.venv/bin/python g1flash.py verify --scan-seconds 30 --device LEFT_UUID --side L --expect stock
# If a test boot needs confirmation and SMP is available:
.venv/bin/python g1flash.py confirm --device LEFT_UUID --side L \
  --firmware build/g1-mvp/stock-app_update.bin
```

Stock verification requires the full diagnostic build date/version and absence
of the CFW marker. It is not satisfied by any arbitrary non-CFW reply. The network
core and bootloader are not uploaded. The tool does not issue raw settings or
calibration writes; stock firmware handles its own runtime-mode bookkeeping.

## Offline acceptance

`tests/test_pipeline.py` runs the production SMP framing/transport against a
simulated GATT peripheral with fragmented notifications and signed MCUboot
images. It covers install, test activation, confirmation, stock restore, lost
upload acknowledgments, upload resumption, lost reset acknowledgments,
corrupted uploads, refusal of unconfirmed/incorrect source images, pending-image
resumption, wrong device selection, diagnostic fragmentation, and worker timeout.
The original tests also validate the actual downloaded stock image and committed
patch set. Simulation establishes host behavior, not G1 hardware compatibility.

## Image evidence

Source: [public G1 firmware link](https://github.com/AGiXT/mobile/blob/main/Even%20Realities%20G1%20BLE%20Protocol.txt),
[vendor CDN archive](https://cdn.evenreal.co/firmware/3adb8ebbd35c2343409d6d0c9fe6cbb9.zip).

| Input | SHA-256 |
| --- | --- |
| Stock ZIP | `40f5839def1c00da3e0ffea9274dc0e04f249596d31787bd0c497be363cf6f8f` |
| Stock application | `1eba87bfb1df261d197ca42e0850cc4ff7e668a9e94e02a8be46369b3bfaf483` |
| Stock network core | `523efcef45a657de827dce2120b5ab1314779e8c51767bee82654559614c2a1d` |
| Stock application signed region | `f4cdaeac1cdecad243aaf1ec64fa778120e8b4f7e2eacf1b25411bcdeb0870f4` |
| Patched application | `8719c710f78f9c4967f83a47e4617f9f9b3b10b69da0100c53a55616737f3177` |
| Patched application signed region | `92c6dfcbced739d4a89b023a0dc2e1907bbcc6cebe78908a0892416acdecf32f` |

The app is 976,509 bytes, with a 512-byte MCUboot header, 975,661-byte body,
and 336-byte TLV area. The ZIP manifest gives vector/load address `0xc200`;
the complete file maps from `0xc000`. The initial vector words are
`SP=0x20031858`, `reset=0x00053e89` (Thumb).

The stock application and network image both authenticate with the public
[MCUboot v1.9.0 RSA development key](https://github.com/mcu-tools/mcuboot/blob/v1.9.0/root-rsa-2048.pem).
The SHA-256 of its DER PKCS#1 public key is
`fc5701dc6135e1323847bdc40f04d2e5bee5833b23c29f93593d00018cfa9994`.
The match was confirmed by verifying the stock RSA-PSS signature, not only
comparing the key-hash TLV. This proves the downloaded artifacts use that key;
it does not prove an unseen device's current bootloader accepts this image.

The stock diagnostic literal is at **file offset `0x95cf9` / CPU address
`0xa1cf9`**. Its pointer is at file `0x3aa4`. Thumb disassembly confirms the `#t`
handler at `0xf9b4` compares two bytes, formats the response at `0xf9f0`, and
sends 200 bytes via the stock callback at `0xf9fa`. The format arguments and
length are preserved. The marker is the only body change.

To intentionally regenerate the profile, download that public development key,
then run `tools/regenerate_marker.py --stock PATH --key PATH` in the venv.
The tool pins the original key file and archive hashes. RSA-PSS uses random
salt, so regeneration changes the signature/output hash; review and update the
profile and this evidence together. Ordinary builds replay the committed bytes.

## Hardware session on 2026-10-28

Both temples were identified by scan and probed before any write:

| Temple | Advertised name | Diagnostic |
| --- | --- | --- |
| L | `Even G1_26_L_C98EE7` | app build `2025-03-21 11:38:43`, `ver 1.5.60`, net build `2025-03-21 11:41:05` |
| R | `Even G1_26_R_48D3D5` | identical |

Both reported the stock signed-region hash `f4cdaeac…0870f4` as the active,
confirmed image in slot 0, with no second slot listed. Battery was 100% on both.
Note the reply ends in `\n\r`, not `\r\n`; the parser accepts it.

Observed transport facts (these supersede the earlier inferences):

- **SMP is exposed in normal mode.** The GATT table has the Nordic UART service
  and the MCUmgr SMP service together, MTU 498. No `4a 01` DFU entry was needed
  and none was sent.
- **The firmware drops an unbonded central 30 s after connecting.** The first
  connection attempt to each temple also timed out once before succeeding.
  In `low_speed_peripheral_dispatch_thread`, the connect callback stamps
  `field_0xae4` and the main loop calls `bt_conn_disconnect(conn, 0x13)` when
  30,000 ms have elapsed, unless a bonded peer address is stored at `0x1080` or
  the device is in DFU mode. Bonding the Mac would unpair the phone
  (`on_pairing_complete` calls `bt_unpair` on a new address), so the tooling
  does not bond. The "disconnect because invalid mac" guard did not fire because
  the glasses were in the open case, which enables allow-bind mode.
- **Uploads resume.** MCUmgr resumes by full-file SHA-256, so `stage` is simply
  rerun after each 30 s drop. `g1flash.py` now records a `disconnected` event and
  fails the pending request immediately instead of waiting out the SMP timeout.
- **480-byte chunks are accepted.** The device negotiates MTU 498, matching the
  `MCUMGR_BT_OTA_DFU_SPEEDUP` sample configuration. `stage --chunk-bytes 480`
  moved about 170 KB per 30 s window; the default 128 moves about 45 KB.
  The CFW staged in six windows (about 3.5 minutes wall clock).
- **Rebooting takes about two minutes to re-advertise** after the reset ACK. A
  scan that fails inside that window is not evidence of a failed boot.
- **The production bootloader is overwrite-only with auto-confirm.** After a
  non-permanent `activate`, the new image came up as active and `confirmed`
  with slot 1 no longer listed. There is no test/revert state and no copy of
  the previous application on the device; the only way back is the OTA restore.

Left temple install result: `verify` observed
`G1CFW/1 net bt: 2025-03-21 11:41:05, app build time 2025-03-21 11:38:43, ver 1.5.60`
and `state` showed hash `92c6dfcb…decf32f` active and confirmed. Session logs
are under `build/logs/` (`20261029T0358*` through `20261029T0405*`).

Left temple restore result: the exact stock application was staged in seven
windows, activated, and came back after about two minutes reporting
`net build time: …, ver 1.5.60` with hash `f4cdaeac…0870f4` active and
confirmed (`20261029T0406*` through `20261029T0412*`). The OTA restore path is
therefore demonstrated on hardware, not only simulated.

The CFW was then reinstalled on the left temple with the same loop (seven
windows, `20261029T0413*` through `20261029T0418*`); `verify` again observed
`G1CFW/1` and `state` showed the CFW hash active and confirmed.

The right temple was installed in parallel with the left reinstall (seven
windows, `20261029T0413*` through `20261029T0419*`) with the same result:
`G1CFW/1` observed, CFW hash active and confirmed. Both temples still answer
the stock battery query (`2c 01`) afterwards. Display, pairing with the phone,
and charging behaviour were not observed from the host and remain for the user
to check by eye.

## Code injection (G1CFW/2 and later)

The marker proved the signing/flash path. Later profiles append compiled code
to the application image, g2flash-style, and hook one stock call site:

- **Toolchain.** `tools/build_blob.py` compiles `cfw/src/g1cfw.c` with Apple
  clang (`--target=thumbv8m.main-none-eabi -mcpu=cortex-m33 -Os -fropi`) to a
  position-independent blob and resolves intra-blob branches and PC-relative
  rodata references itself; any other relocation is an error, so stock entry
  points are called through absolute addresses from `cfw/src/fw_abi.h`.
- **Placement.** The primary slot is `0xc000..0xfc000` (flash map in the
  image at file `0x826d0`). The stock image already ends at `0xfa67d`, and the
  last sector is the MCUboot trailer, so the blob is appended at `0xfa530`
  (word-aligned end of the signed body) with about 2.3 KB total headroom. The
  header image size, SHA256 TLV, and RSA-PSS TLV are regenerated;
  `cfw/image.py` enforces the budget and reproduces the committed output hash.
- **Hook.** `0x2338e` in the BLE receive entry (`bl spec_ble_command_hook`)
  becomes `bl cfw_rx`. Non-`0xF0` packets tail-call the stock hook with the
  original arguments. Replies use the context send callback at `ctx+0xc` from
  the getter at `0x1880c`, exactly as the stock `#t` reply does.
- **Display.** `GlassesState` (getter `0x16d3c`) embeds the panel context at
  `+0xb6c`; the frame pointer is at panel `+0x344`, panel origins at `+0x348`
  and `+0x34c`, and the power flag at `+0x35c`. `_reflash_fb_data_to_lcd` is at
  `0x4a22c`; `global_system_suspend`/`global_system_resume` (panel power with
  the state byte at `+0xfea`) are at `0x2d78c`/`0x2d808`. See [docs/G1_CFW_PROTOCOL.md](G1_CFW_PROTOCOL.md) for the commands.
- **Carving (G1CFW/7 and later).** Stock behaviour is not required, so the
  blob now overwrites stock code instead of appending. `tools/g1map.py` sweeps
  the binary for function prologues and names 354 functions from their own log
  strings (`build/g1-1.5.60-functions.json`); `tools/g1carve.py LO HI` lists
  every external way into a range (branches, pointer literals, movw/movt) so a
  carve has an explicit stub list. `DashBoard_Reflash` (`0x399e0`, 15,828
  bytes, called only from `ui_DashBoard_task`) is the first region: a 4-byte
  `movs r0,#0; bx lr` stub at its entry and the blob at `0x399e4`, leaving the
  image at the stock size. Further candidates from the map: the UI task block
  `0x399e0..0x42da8` (about 38 KB, 80 entry points), notification parsing
  (about 10 KB), navigation map rendering (about 4 KB), factory tests (about
  2 KB). The `gui_utf_*` text drawers (about 10 KB) are worth keeping for a
  built-in-font text command.
- **Regenerating.** `tools/gen_profile.py --carve dashboard --upgrade-from profiles/...`
  needs clang and the public MCUboot development key (`build/keys/`), rebuilds
  the blob, patches, signs, and writes `profiles/g1-1.5.6-cfw.json`.
  `./build_cfw.sh` then replays every profile under `profiles/` without a
  compiler into `build/<profile>/`. The profile lists older image hashes it may
  be installed over, so `g1flash.py` accepts an upgrade from the marker CFW.
- **Tests.** `tests/host/g1cfw_host_test.c` compiles the injected source for
  the host with stubbed stock entry points and a guarded frame buffer under
  AddressSanitizer/UBSan; `tests/test_cfw.py` checks the committed profile's
  blob placement, hook encoding, and that nothing else in the body changed.
- **Analysis.** `tools/g1dis.py` disassembles the exact 1.5.60 binary and finds
  cross-references, which is how every address above was verified. The 1.6.3
  decompilation under `src/` is reference material only; its addresses differ.

### Hardware result for G1CFW/2 (2026-10-28)

The ping-only profile (`profiles/g1-1.5.6-cfw2-ping.json`, 148-byte blob) was
installed on the left temple over the marker image in seven windows, activated,
and came back with image hash `c8c5683b…e3e2cb` active and confirmed. The stock
`#t` reply still arrived through the hook (`G1CFW/2 net bt: …`), and
`g1flash.py ping` received `f0 00 00 "G1CFW/2" 00 01 05 3c`: the appended code
runs, the receive hook passes stock traffic through, and the reply callback
convention is correct. Logs: `20261029T0444*` through `20261029T0452*`.

### Hardware results for G1CFW/3 and G1CFW/4 (2026-10-28)

- G1CFW/3 (display commands) installed over G1CFW/2 and answered PING with
  `G1CFW/3`. INFO reported the frame buffer at `0x20059115`, column origin 0,
  row origin 210, and the panel unpowered. A full-frame FILL and a 67-packet
  1bpp test-pattern BLIT were accepted; FLUSH correctly refused with
  `panel-off`, so the frame-buffer path works but nothing was visible while
  docked. Logs `20261029T0452*` through `20261029T0501*`.
- G1CFW/4 added PANEL via `global_system_suspend`/`global_system_resume`.
  Suspend moved the stock power byte to 1 and resume back to 0, but the panel
  flag at context `+0x35c` stayed 0: `panel_resume` only calls `panel_on` when
  bit 8 of the flag halfword at state `+0x1070` is set, which the stock
  firmware clears while the glasses are docked. Logs `20261029T0505*` through
  `20261029T0512*`.
- G1CFW/5 therefore adds PANEL modes 2/3 that call the panel context's own
  `on`/`off` callbacks (`panel_on` at `0x49e48`, `panel_off` at `0x49d9c`),
  bypassing the dock policy for development.

### Hardware result for G1CFW/5 (2026-10-28)

Installed over G1CFW/4 on the left temple while docked. PING answered
`G1CFW/5`; `panel --force-on` returned the panel flag set (stock power byte
still 1, i.e. the system considers itself suspended); `draw --clear` cleared
the frame, sent the 67-packet 1bpp test pattern, and the full-frame FLUSH
succeeded; INFO twenty seconds later still showed the panel powered. Host-side
evidence ends at the SPI push; whether the pattern is visible on the lens
needs a look at the glasses. Logs `20261029T0513*` through `20261029T0521*`.

### Visual check and G1CFW/6 (2026-10-28)

The user compared the left lens against a reference rendering of the same
pattern: everything matched except the right edge of the border, which was
cut off. Cause: `_reflash_fb_data_to_lcd` sends `x1/2 - x0/2` bytes per row,
so its x1 is exclusive while y1 is inclusive; passing `x0 + w - 1` drops
columns 638-639, exactly as the stock full-frame refresh does with `0x27f`.
G1CFW/6 passes `x0 + w` (the routine clamps at `0x280`, so 640 is accepted).

### Hardware result for the carve and display stubs, G1CFW/8 (2026-10-28)

The left temple booted the carved build (blob at `0x399e4` inside the
stubbed `DashBoard_Reflash`, plus stubs on `display_reflash_handler` and the
whole-frame clear), answered PING with `G1CFW/8`, powered the panel and drew
the full-width test pattern. The image is back to the stock size with 14.5 KB
of room in the region. Note for the install loop: the stage command re-reads
the firmware file on every reconnect, so a rebuild during a loop changes what
is uploaded mid-way (the final hash check still guarantees a consistent image);
copy the file before looping.

### Hardware result for G1CFW/9: context, stream, RLE (2026-10-28)

Left temple, docked, panel forced on, full-frame test pattern with clear and
flush, measured by `g1flash.py draw` (host wall clock including replies):

| Path | Packets | Bytes | Time |
| --- | --- | --- | --- |
| Per-packet BLIT with a reply each | 67 | 16,000 | 5.48 s |
| STREAM, pipelined, 4 messages | 66 | 16,072 | 1.87 s |
| STREAM with PackBits RLE, 2 messages | 22 | 5,152 | 0.43 s |

STATS afterwards: 6 messages, 0 errors. Write-without-response packets sent
back to back were not dropped by the stock receive path at this rate. The
right temple then took G1CFW/9 the same way and drew the RLE frame in 0.65 s
with 0 errors. Both temples are on G1CFW/9 (image hash `db29379a…4fdfb6`).

### Hardware result for G1CFW/10: text (2026-10-28)

Both temples took G1CFW/10 (image hash `5917e8e9…8491c4`). `text "Hello G1
left"` in font 0 at (300,10)-(636,37) and `"12:47"` in font 1 at
(300,60)-(636,100) returned ok on the left temple, `"Hello G1 right"` on the
right, drawn over the RLE test pattern; STATS showed 0 errors. The left
temple's first activation attempt missed its scan window after the staging
disconnect, so the install loop now retries activation. Legibility of the
glyphs is for the user to confirm by eye.

### Hardware result for G1CFW/11: copy (2026-10-28)

Left temple: `copy --rect 300,10,336,28 --to 300,110` returned ok after the
text draw, so the text box should now appear twice (at y 10 and y 110);
STATS 0 errors. The right temple did the same. Visual confirmation pending.

### Hardware result for G1CFW/12: heap arena (2026-10-29)

Both temples booted G1CFW/12 and every asset command returned `no-memory`:
the stock `malloc` (0x7a15c) refused the 48 KB arena request, and ASSET_INFO
showed nothing allocated. The heap already holds the 64 KB frame buffer and
the stock upload queue. G1CFW/14 retries in 8 KB steps down to 8 KB and
reports the size it obtained.

## References and remaining work

- Decompilation pinned at `f571782aedb6fdefbcbfe2a4d1e309bb47143581` (1.6.3 reference).
- Local g2flash inspected at `1a6db4fd75de202a96599cc8f9f12fa721e7c3ce`.
- [MCUboot image format](https://docs.mcuboot.com/design.html#image-format).
- [Zephyr SMP framing](https://docs.zephyrproject.org/latest/services/device_mgmt/smp_protocol.html)
  and [image management](https://docs.zephyrproject.org/latest/services/device_mgmt/smp_groups/smp_group_1.html).
- [G1 protocol wiki](https://github.com/JohnRThomas/EvenDemoApp/wiki/Even-Realities-G1-BLE-Protocol).
- REA source consulted at `e79ad6ca8ebbf2e96b5095e7c2b8bfaf27ca7559`,
  [raw import documentation](https://github.com/morluto/rea/blob/main/docs/raw-binary.md).
  Its generic ARM mapping does not establish Cortex-M33/Thumb context; the
  existing decompilation and explicit Thumb disassembly were enough for this
  data-only patch. No REA engine installation was required.

Both temples boot the marker CFW and the OTA restore path has been exercised
on the left temple. Remaining acceptance work is visual: confirm the display,
phone pairing, and charging behave as stock on both temples.

After that, the smallest useful graphics backport is one bounded, opt-in bitmap
command using the G1 panel path. Recover its addresses from the exact target
binary, establish RAM/flash budgets, and exercise the handler under host tests
before adding an injected code hook. G2's Apollo/FreeRTOS addresses, 4bpp shadow,
and private SID framing cannot simply be used on G1's nRF5340/Zephyr image.
