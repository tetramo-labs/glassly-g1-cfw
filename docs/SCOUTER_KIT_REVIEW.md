# Scouter Kit comparison — 2026-11-05

Reviewed [mafik/scouter_kit at 8cb6334](https://github.com/mafik/scouter_kit/tree/8cb6334b54ce48ae670c5be7fe6da9e5b7f409e0),
against our G1CFW/44 source and pinned 1.5.60 image. Scouter Kit replaces the
application with a Zephyr firmware and VM; ours retains the stock runtime and
injects code into a reclaimed region. Its peripheral findings are useful, but
its drivers and addresses are not drop-in patches. Its reported hardware results
are upstream evidence, not tests performed on our glasses.

## Implemented in G1CFW/45

The comparison prompted three independently verified fixes in our code:

| Fix | Before | After |
| --- | --- | --- |
| STREAM replay result | A CRC/sequence error overwrote `last_status`, which ONCE reused for an earlier executed message. | A separate byte in existing context padding retains that execution's result, including deferred and partially failed messages. |
| Host acknowledgement matching | Any STREAM reply could complete the next send; unrelated replies restarted its timeout. | Match the current sequence range, require the final packet for success/deferred, and keep a single deadline. Earlier-packet errors still fail immediately. |
| Vertical panel clipping | VIEW accepted offsets through ±480, but flush passed off-panel rows to the stock driver's nine-bit address mask. | Clip destination rows to 0..479 while retaining source canvas coordinates. Fully hidden rectangles make no SPI transfer; framebuffer data survives. |

Scouter Kit's [reliable transport contract](https://github.com/mafik/scouter_kit/blob/8cb6334b54ce48ae670c5be7fe6da9e5b7f409e0/g1_firmware/docs/VM.md#62-packets-and-reliability)
motivated the replay/acknowledgement audit. Its
[panel driver](https://github.com/mafik/scouter_kit/blob/8cb6334b54ce48ae670c5be7fe6da9e5b7f409e0/g1_firmware/firmware/src/panel.c)
made the physical-window bounds explicit. Locally, `tools/g1dis.py dis 0x4a22c`
confirms that 1.5.60 constructs row addresses with mask `0x7fc00` at
`0x4a2dc..0x4a2de`; it does not clip to the 480-row panel.

No upstream code was copied. The wire layouts stay compatible. ONCE still
identifies a message by length and CRC-16; this patch does not introduce unique
request IDs, automatic retransmission, or protection against CRC collisions.

## Hardware follow-ups, in priority order

1. **Check the rightmost pixel on the actual panel.** Upstream's `panel.c`
   documents four trailing dummy clocks: N payload bytes store 2N−1 pixels.
   It sends a separate tail window for an even-width rectangle's last pixel.
   Our stock `0x4a22c` sends a five-byte header plus `width/2` bytes, despite
   temporarily zeroing the byte after that range. This is a strong lead for a
   stale last column, separate from /44's concurrent-header corruption fix.
   Confirm the complete SPI path and compare alternating last-column patterns
   with panel readback before adding a tail transfer. Sending an arbitrary extra
   byte can overwrite the neighbouring pixel or wrap into the next row.
2. **Charging-case interference.** Upstream reports lit pixels disrupting NFC
   case polls and blanks LEDs around communication; see the 0.5.32 and 0.5.60
   [development notes](https://github.com/mafik/scouter_kit/blob/8cb6334b54ce48ae670c5be7fe6da9e5b7f409e0/g1_firmware/docs/DEVELOPMENT.md).
   Our automatic policy already turns the panel off in case work modes 5/6,
   but manual PANEL commands can bypass it. Trace the 1.5.60 case path before
   adding blanking that must coexist with stock brightness writes.
3. **Microphone lease.** Upstream's 0.5.12 firmware expires capture unless the
   host renews a two-second lease. Our explicit MIC_CONTROL start/stop has no
   CFW lease, so a host crash can leave capture dependent on stock cleanup.
   Add an opt-in lease with client renewal and disconnect tests; changing the
   existing start command unconditionally would break current phone clients.
4. **Display power and latency.** Upstream measures roughly 1 mA for standby
   and batches panel writes in eight-row windows. Our margin blank performs
   280 separate stock calls, each with a 1 ms end wait, then repeats at 600 ms
   and 1.2 s. A dedicated bounded clear/flush driver could shorten wake and
   lock time. Current firmware has only 12 bytes of flash headroom, so this
   needs a separate space/reclamation investigation and hardware validation.

The upstream battery/current guard is also relevant to sustained bright scenes,
but its thresholds were measured on that project's hardware and full-panel
workload. Porting them without checking our stock PMIC handling would be premature.

## Build and validation

The current profile is G1CFW/45. G1CFW/44 is preserved in
`profiles/g1-1.5.6-cfw44-panel-lock.json`; historical upgrade hashes are retained.
The new blob is 23,984 bytes in the unchanged 23,996-byte carve. No new RAM,
partition, network-core or hardware-driver changes are required.

```sh
./build_cfw.sh --profile profiles/g1-1.5.6-cfw.json --stock build/g1-mvp/stock-firmware.zip
G1_STOCK_ZIP=build/g1-mvp/stock-firmware.zip .venv/bin/python -m unittest discover -s tests -v
```

Regression coverage includes stale acknowledgements, sequence wrap, an error on
an earlier packet, fixed timeout under stale traffic, replay after corrupt
retries for three execution outcomes, and partially/fully clipped rows with
framebuffer restoration under horizontal shifts. The C harness uses AddressSanitizer
and UndefinedBehaviorSanitizer; existing shape/vector replay and signed-image
tests also run. All 79 tests passed with the pinned stock ZIP. The compiled
blob matches the pinned profile and rebuilding reproduces the signed image
exactly; upgrade history and stock restore eligibility were checked for /44
and /45. The image was built and signature-verified locally; it has not
been flashed, run in Scouter Kit's emulator, or hardware-tested.

Output: `build/g1-1.5.6-cfw/app_update.bin` (976,509 bytes).
File SHA-256: `7a2674059de3946aeb104255bba23ce9980944e0ab86430dafc09023b25788cc`.
