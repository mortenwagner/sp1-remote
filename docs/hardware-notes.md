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

## Sync jack

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
