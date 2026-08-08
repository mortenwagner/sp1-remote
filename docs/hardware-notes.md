# Hardware notes

Bench-verified facts about pop. Measured or observed only: anything inherited
from someone else's source is marked as such.

## Observed on the looper (baseline for "an app is running")

Recorded 2026-08-08 while running `sp1_looper.bin`:

- **Boot:** an LED animation plays on startup.
- **Power-off:** a sweep animation on the LEDs "on the side".

Both come from the looper's own code, and both are in the region being
transplanted (its boot sequence runs an LED sweep, and `power_off()` clears
the rows through an animation). Consequence worth remembering: our Phase 2
skeleton deliberately does a plain 1 Hz blink on one LED instead, so the
absence of an animation is the fastest way to tell our firmware from the
looper at a glance.

The phrase "on the side" is a placement clue for the LED survey (Task 2.3):
the eight known LEDs are four in a centre row and four above the track
buttons, and at least some of them read as side-mounted from the front.
Confirm which index is where during the survey.

## Measured on pop, 2026-08-08 (Phase 2 skeleton, raw log in bench-2026-08-08-pop.txt)

### Button ladder: every transplanted threshold confirmed

| Control | Looper's comment | Measured on pop |
|---|---|---|
| Track 1 | ~213 | 216-224 |
| Track 2 | ~403 | 405-415 |
| Track 3 | ~733 | 740-747 |
| Track 4 | ~1220 | 1226-1234 |
| Play | ~1823 | 1830-1836 |
| Track 1 + 4 combo | ~1325 | 1322-1329 |

**The recovery combo does not collide with track 4.** An isolated track-4
press peaks at 1234; the combo band starts at 1280. 46 counts of margin, and
the combo lands mid-band. Holding track 4 alone can never reach DFU, and the
combo can never be read as a track-4 press. This was the highest-severity
finding of the second review pass, and it is now closed on hardware.

### Faders: absolute position, and full scale is NOT 3700

All four strips sweep 0 to their maximum under a finger, so they are true
absolute-position sensors rather than relative gesture strips (the spec's
open risk, closed).

| | f0 | f1 | f2 | f3 |
|---|---|---|---|---|
| max | 3742 | 3735 | 3735 | 3734 |

`FADER_RAW_FULL` is therefore **3730**, not the looper's 3700, which would
have clamped the top ~1% of every strip's travel to 127. 3730 sits just below
the lowest observed maximum so every fader can still reach 127.

### Fader jitter is worse than the looper claims

The looper's source says the ADC jitters plus-or-minus 1 count. Measured on
pop: **4 to 7 counts peak-to-peak** in a quiet window, and **up to 23** over
tens of seconds on a strip parked near its rail. One 7-bit MIDI step is about
29 counts, so noise of that size can flap a fader resting on a bucket
boundary between two CC values.

Two responses, both applied:
- `FADER_DEADBAND_RAW` raised from 8 to **12** (still under half a step).
- ADC oversampling raised from 2 to **4** samples. The looper capped it at 2
  because its blocking reads competed with an eMMC streamer; this firmware
  has no audio or storage, so the CPU is free and averaging 4 halves the
  noise for about 0.5 ms per control pass.

The acceptance test is Phase 4's: a puck left alone must emit nothing.

### LED placement

`leds[0]` (P1.13) is the **top of the four side LEDs**. So what the looper's
source calls the "centre row" is physically the vertical column on the side.

### Escape hatch

Track 1 + Track 4 held ~1.2 s from our running firmware reset pop into the
bootloader, confirmed by the USB identity reverting to `stem player`. The
hatch works.

## USB MIDI verified on the Mac, 2026-08-08

Pop enumerates as two CoreMIDI ports, `SP-1 Remote Block 1` (MIDI 1.0
compatible) and `SP-1 Remote MIDI 2.0`, alongside the CDC console. The
composite binds correctly.

Listening on Block 1 while the Phase 3 smoke ramp ran:

```
ch1 CC 102 = 71, 67, 63, 59, 55, 51, 47, 43, 39, 35, 31, 27 ...
CC102 values: min 0  max 127   rate 8.8/s
```

Channel, CC number, full 0-127 range, step size and rate all match what the
firmware emits. That proves the coalescing queue, the drain thread, the USB
sink and the composite device in one measurement.

Reading it from the command line needs a little care: `receivemidi` is not in
core Homebrew and its tap fails to build on this Xcode setup. What works is a
throwaway venv with `python-rtmidi`, which sees every CoreMIDI port.

**The TRS sink remains unverified.** It shares the same queue and the same
pacing, so everything upstream of the two sinks is now proven; what is
untested is only the bit-banged jack itself and the adapter it needs.

## Phase 4 verified end to end, 2026-08-08

All four strips drive their profile CCs, measured on the Mac over USB MIDI:

| CC | fader | msgs | range | channel |
|---|---|---|---|---|
| 102 | 1, cutoff | 101 | 0-127 | 1 |
| 104 | 2, reverb wet | 87 | 0-127 | 1 |
| 107 | 3, delay time | 98 | 0-127 | 1 |
| 108 | 4, delay feedback | 101 | 0-127 | 1 |

No missing CCs, none unexpected, peak 63 messages/s against a 200/s cap.

### The strips LATCH their position

Settled by direct test: fader 1 left at mid-travel read 2221 and held for
20 seconds hands-off, drifting +2 counts. They are latching absolute-position
sensors, not touch sensors that fall to zero on release. Our control model
needs no touch detection.

This was not obvious. Values genuinely do fall to near zero, but only because
the BOTTOM of the strip reads 0-3, the same as an untouched strip that was
last left at the bottom. A finger travelling down to the bottom and lifting
looks identical to a decay, which is what sent me chasing a touch model that
did not exist.

Jitter at mid-travel, hands-off: 2218-2228, so 10 counts peak-to-peak. The
deadband of 12 sits just above it, and 55 seconds of hands-off listening
produced zero spurious MIDI messages.

### Debugging note worth keeping

An empty MIDI capture is NOT evidence that the firmware is silent. Several
captures here returned nothing purely because they did not overlap with the
sweeps, and each empty result invited a wrong theory. What settled it in one
line was instrumenting the firmware itself with counters for messages pushed,
drained, handed to USB, and whether the host had the interface open:

```
push=886 drain=886 usbtx=886 rdy=1
```

Four numbers advancing in lockstep proved the whole transmit path was healthy
and moved the search to the listener, where the problem actually was.
Instrument before theorising.

## Phases 5 and 6 verified, 2026-08-08

**Freeze (CC 64), 12 messages:** `127, 0, 127, 0, ...` strictly alternating,
zero repeated values, including a run of deliberately fast presses. Every
press registered. The toggle design plus the three-pass debounce is solid on
this noisy shared ladder, and a missed press would have shown here as an
inverted state.

**Shimmer (CC 105), 7 messages:** `16, 112, 80, 48, 16, 112, 80`, which
decode against popgoblin's ShimmerLevel enum as Boost, Off, Low, Full,
Boost, Off, Low. It began on Full, where the synth boots, and cycled in the
spec's off/low/full/boost order with every value landing mid-bucket. This
confirms on hardware the inverted-enum fix from the first review pass: the
original {0, 42, 85, 127} would have had Off and Boost swapped.

No unexpected CCs in 330 messages.

## Presets work, 2026-08-08

Hold-to-save writes and confirms with the three fast blinks, and the scene
**survives a power cycle** and recalls correctly. Save, persist and recall
all proven, so this is real flash storage rather than a RAM copy that
happens to still be there.

It is also the first successful write to the relocated storage region at
0xF7000, which validates moving off the bootloader settings page at
0xFF000, where the safety gate had correctly refused.

## TRS MIDI PROVEN, 2026-08-08

With the UARTE rewrite (data on the TIP, P0.20, Type A) and an **ordinary
3.5 mm TRS cable** into a USB MIDI interface. Captured both paths at once
and compared, TRS against the already-trusted USB sink:

| CC | TRS | USB | |
|---|---|---|---|
| 64 freeze | 6 | 6 | identical |
| 102 cutoff | 230 | 229 | 1 extra at the capture edge |
| 104 reverb wet | 33 | 33 | identical |
| 105 shimmer | 2 | 2 | identical |
| 107 delay time | 29 | 29 | identical |
| 108 delay feedback | 70 | 70 | identical |

370 vs 369 messages, zero malformed, channel 1 throughout. The single
difference is the capture loop polling two ports in sequence and closing
mid-stream, not a transmission fault.

**No adapter was needed.** Everything the earlier plan said about building
a tip/sleeve-swap adapter, and the whole multimeter investigation in Task
1.2, was predicated on the looper's claim that MIDI leaves on the RING via
P0.23. feldd's firmware says the tip at P0.20, validated against an OP-XY,
and that is what works here. The looper's P0.23 drives Pocket Operator sync
instead.

Consequences: Task 1.2 (source resistance, drive current, adapter) is moot,
and so is the `MIDI_INVERT` polarity unknown. A hardware UART also removed
two Codex findings, flash writes corrupting an in-flight byte and USB
interrupts jittering bit edges.

## Sync jack (superseded, kept for the record)

Not yet measured; the multimeter work (Task 1.2) is deferred. What is known:

- **Inherited, unpublished:** MIDI data leaves on the **ring**, driven by
  P0.23 (BC807 base) through a PNP that inverts. The tip carries Pocket
  Operator sync (P0.20 / P0.17). Source: sp1-tape-looper main.c:4405-4410.
  These pins appear in no published document: SP-1-dev's pin header stops
  short of this jack and its wiki page for it is a Todo.
- **Established 2026-08-08:** the receiving end is TRS **Type A** on both the
  Blokas Midihub and the Tiliqua. Push 3 drives both today, and Push 3's
  3.5 mm MIDI ports are Type A, MMA-compliant, with Ableton stating that
  Type B adapters do not work.
- **Therefore the adapter is:** ring straight through, tip and sleeve
  swapped at one end, SP-1 tip unconnected. One adapter serves both
  receivers. No standard part does this: Type A/B adapters swap tip and
  ring, not tip and sleeve.
- **Still unknown:** source resistance and available drive current, i.e.
  whether the SP-1 can push the ~5 mA a standard opto input needs, and
  whether it has enough series resistance to survive a plug-insertion short.
  That is what Task 1.2 measures when a multimeter is to hand.
