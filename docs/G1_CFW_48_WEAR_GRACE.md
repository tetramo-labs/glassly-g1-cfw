# G1CFW/48: wait out a take-off

Revision 48 stays on stock 1.6.3. It keeps the panel on through brief dips
in the wear signal while the glasses are on a head. The implementation passes
offline tests; behavior on a head needs confirmation on glasses.

## Cause and change

Before revision 48 the panel went off on the first 200 ms checker pass that
said taken off: a wear word at or below `wear_off` (53) while stock said worn,
stock mode 4, or the stock `taken off` event. The network core refreshes the
word about every six seconds. On a head it normally reads 62-74, but a single
low sample was enough to blank a worn pair.

The CFW now counts consecutive taken-off passes in `cfw_input.off_ticks`.
That byte used to be struct padding, so `sizeof(cfw_input)` and the view
state behind it do not move. The magic changes to `INP7` so a retained
revision 47 block is not reused. The panel goes off after 100 passes (20 s).
Any other pass resets the count: a worn word, a word of 0 (no statistics),
the dead band between the thresholds, or any mode other than 4. The `worn`
event and INPUT_CONTROL bit 4 also reset it. Mode changes and the `taken off`
event no longer switch the panel off themselves; only the passes count. Case
open/closed (events 8 and `0b`, modes 5 and 6) and deep sleep (mode 8) still
switch off at once. Turning the panel on is unchanged and immediate.

## Artifact and checks

The signed application is still 970,085 bytes. The injected blob plus helper
uses 24,132 bytes, leaving 724 bytes in the carved flash region. RAM use is
unchanged.

- File SHA-256: `ef15c7ae65a0b51f70e7d53ae60a12e673326f0806886a89588899533af982a8`
- MCUboot image hash: `d901885506f3998f5f766077f8d1acb9cd3319e069bfc4132738653a22c36ba6`
- G1CFW/47 and every earlier accepted base remain upgrade sources.

All 90 firmware tests pass. The host test now covers a 10 s dip that leaves the
panel on, and a worn reading, a dead-band reading, a `worn` event and bit 4
each restarting the wait. It also checks that mode changes do not count as
passes, that 20 s of mode 4 or of a low word turns the panel off, that case
and deep sleep switch off at once, and that worn switches on at once.
