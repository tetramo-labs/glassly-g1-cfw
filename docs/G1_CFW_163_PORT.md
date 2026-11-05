# G1CFW/46: stock 1.6.3 port

This records the baseline revision 46 port. The current revision 47 adds the
[panel wake fix](G1_CFW_47_PANEL_WAKE.md); its output hashes and memory usage
are recorded there. The pins and validation counts below describe revision 46.

The CFW uses the exact Even G1 **1.6.3** application, build
**2025-06-28 17:28:42**. Revision 46 retains the display, input, microphone,
IMU and STREAM features of revision 45. It changes the stock ABI, injection
sites and borrowed memory. Offline validation passes; this port has not been
flashed or tested on glasses.

## Pinned inputs and output

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| Vendor ZIP | — | `2bcff7616c3508d41092b7cd3c93640d8bc29a57c76579a930b1220aee16d412` |
| Stock application | 970,085 | `4b366586d895abdd22c5b78cd433b40f64237cbb0b5a6a8f93a3166964cf52aa` |
| Stock network core | 226,020 | `2fdd2862ba5c976ada4b3604c61dbe29cf6ac7d4c10caffebacdb66cd9028b6e` |
| G1CFW/46 application | 970,085 | `1369c45207bd16d5bba831f65c2c943d1503ddaa676bf16dde7953e0b004f094` |

The CFW MCUboot signed-region hash is
`219d2469eb3da344116fc2ff8dff81c20e87162fe192370e505d6ebca0705946`.
The stock application's is
`d7175591bbc0be3872348dd0a5903c0f935e8a1414c926d1f0662f3d6a787ec6`.
Both stock images and the generated CFW pass RSA-PSS signature verification
against the pinned MCUboot development public key.

The output remains **application-only**. The 1.6.3 vendor network image differs
from the old package and is validated as an input, but is not included in the
CFW update. Compatibility with the network core already on each temple still
needs hardware validation, including when upgrading from 1.5.60.

## Reproduce

```sh
# Supply the exact vendor ZIP above; no verified download URL is recorded.
./build_cfw.sh --stock /path/to/even-g1-1.6.3.zip
.venv/bin/python g1flash.py plan --firmware build/g1-1.6.3-cfw/app_update.bin
.venv/bin/python g1flash.py plan --firmware build/g1-1.6.3-cfw/stock-app_update.bin
G1_STOCK_163_ZIP=build/g1-1.6.3-cfw/stock-firmware.zip \
  G1_STOCK_ZIP=build/g1-mvp/stock-firmware.zip \
  .venv/bin/python -m unittest discover -s tests -v
```

Default builds replay `profiles/g1-1.6.3-cfw.json`. They use a verified local
`build/even-g1-1.6.3.zip` or the output directory's cached archive, otherwise
require `--stock`. Historical 1.5.60 profiles remain available with explicit
`--profile profiles/g1-1.5.6-cfw.json` and the matching stock package.

For developers regenerating the injected code and signature:

```sh
.venv/bin/python tools/gen_profile.py --carve dashboard \
  --stub display --stub link --stub input \
  --upgrade-from profiles/g1-1.5.6-cfw.json
```

This requires clang and the pinned development private key. Regeneration uses
RSA-PSS randomness; the committed profile provides deterministic replay.

## ABI and control flow

`cfw/targets/g1-1.6.3.json` records the input hashes, 47 instruction/literal
guards, carve boundaries, call sites, hooks and RAM regions. The generator
checks these against the stock bytes before compiling. `cfw/src/fw_abi.h`
contains the corresponding callable addresses and structure offsets. Addresses
below are even instruction addresses; C function pointers set the Thumb bit.

| Role | 1.6.3 address |
| --- | --- |
| BLE receive hook call | `0x23d4e` |
| Stock command hook | `0x0ef28` |
| Context / GlassesState getter | `0x18320` / `0x1681c` |
| Panel rectangle transfer | `0x4abd0` |
| LCD lock / unlock | `0x4af50` / `0x4af60` |
| System suspend / resume | `0x2d8f8` / `0x2d974` |
| Panel suspend / resume | `0x4aea8` / `0x4aef0` |
| Brightness write | `0x4a830` |
| UTF drawing / rows / flush | `0x462e8` / `0x452f0` / `0x45428` |
| Allocation / display post | `0x7a014` / `0x80fca` |
| Sleep / semaphore give / BT queue | `0x7fef6` / `0x7feca` / `0x17fec` |
| Display / touch / stock event hooks | `0x4c238` / `0x2c278` / `0x29774` |
| Work-mode entry / check hooks | `0x169c0` / `0x297e0` |

The work-mode checker now saves `{r4,r5,r6,r7,r8,lr}`. Replaying the old
1.5.60 eight-register push would corrupt its return stack. The naked detour
matches the new push and resumes at `0x297e4`. The work-mode entry repeats
its push/move and resumes at `0x169c4`. The suspend detour repeats the push
and `ldrb [r0,#0xfea]` before resuming at `0x2d8fe`.

The dashboard entry at `0x38dc4` becomes `movs r0,#0; bx lr`. The region
`[0x38dc8,0x3eee0)` provides 24,856 bytes for 23,988 bytes of injected code
and constants plus a 12-byte relocated helper, leaving **856 bytes**.
Three BL callers become `movs r0,#0; nop`; three B.W dispatch tails become
`movs r0,#0; bx lr`. Tail calls cannot safely be replaced with a NOP.

Four live BLE paths call the quick-note reset helper at `0x3d7b4`, inside the
carve. Its 12 bytes (including the PC-relative literal `0x20004b20`) move to
a word-aligned location after the blob; all four callers are retargeted.
The helper still clears the byte at `0x20004b32`. Cross-reference inspection
found the remaining apparent pointers into the carve were instruction words
or a numeric lookup table, not callable function pointers. Decompiled labels
alone were insufficient: the dashboard task's recorded label points into a
literal pool; its binary entry is `0x3cdb4`.

## RAM ownership

| Region | Range (end exclusive) | Owner disabled by CFW |
| --- | --- | --- |
| Message/asset state | `0x20017d30..0x20019130` | Stock BMP uploads (`15`, `16`, `20` swallowed) |
| Objects/animation/input state | `0x2001c750..0x2001db40` | Stock navigation/translation display dispatch |
| Saved panel edges | `0x200143d0..0x20015774` | Stock dashboard/navigation drawing |

Region 2 is **0x13f0 bytes**, smaller than in 1.5.60: the live stock word at
`0x2001db40` must remain outside it. Current state consumes 5,096 of its 5,104
bytes. Compile-time bounds and host tests with surrounding canaries check the
limit. Region 3 ends before the adjacent stock buffer at `0x20015777`.

Touch event/semaphore/ready move to `0x200084f8`, `0x200084fc`, `0x20019a58`.
The wear word is `0x20006bc8`, the wear setting is state+`0x10d4`, and IMU
attitude words start at `0x20008484`. Microphone arming is state+`0x106d`,
with flash routing at `0x20008558`; the main semaphore remains state+`0x68`.
Panel context/frame/viewport and event-context offsets remain unchanged.

Host raw diagnostics select the RAM layout from PING's base version, reject
unknown bases and read the allocated GlassesState pointer from `0x20006bdc`
(`0x20006b9c` on 1.5.60). They no longer assume a fixed heap address.

## Validation and remaining hardware checks

The 89-test offline suite covers host C execution under ASan/UBSan, protocol
regressions, exact source-to-profile compilation, stock ABI guards, signed
patch replay, detour stack/resume instructions, tail-call stubs, helper
relocation, base-specific host reads, and pinned upgrade/restore plans.
The new profile accepts the previous stock/CFW identities and preserves the
older upgrade history. Restoring either pinned stock base remains possible
through explicit offline plans.

Glassly validation also passed the Android SDK build and 63 G1 unit tests,
plus the iOS SDK/test build and 61 G1 simulator tests. The built Android AAR
and iOS resource bundle contain the exact signed CFW image, matching both
platform catalogs.

Before treating this port as hardware-validated, check both temples: boot and
identity (`G1CFW/46`, `1.6.3`), reconnect, display/animation, wear and charging
power transitions, touch/events, streamed IMU, microphone audio, and stock
restore. Historical hardware results for 1.5.60 do not validate this port.
