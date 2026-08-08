# Fable 5 independent review, 2026-08-08

Second adversarial pass, run after the Codex review had been folded in, so
it reviewed the *fixed* plan rather than the original. Brief: prioritise
the freeze revision (newest, least reviewed), then whole-plan coherence,
then hardware-damage risk, then the C.

It verified every transplant citation against the looper source and the
shimmer / CC 64 claims against the tiliqua source, and confirmed the
post-Codex fixes hold. Five new findings, all folded in.

The one that mattered:

**The recovery combo had become a destructive write.** The looper detects
Track 1 + Track 4 as its own raw ADC band (1280-1390) and checks it BEFORE
decoding buttons, with the explicit comment "so the combo isn't mistaken
for a Track-4 press" (main.c:6964-6981). This plan's decode maps 950-1500
to track 4, which the profile assigns to preset B. So the Codex round added
enter_dfu() to the transplant list, but no main.c listing ever called it,
and the documented rescue gesture would instead have fired hold-to-save:
a flash write that destroys the stored scene, with DFU never reached.
Fixed by a pre-decode band check with its own 1.2 s timer, plus a rack test
that the combo reaches DFU without touching preset B.

The others:

- Task 3.2 (transmit queue) used cc_msg_t from profile.h, which Task 4.2
  creates a phase later, so it could not build. cc_msg_t now lives in its
  own cc_msg.h. The same task's "create empty stubs" instruction also
  contradicted every later "Expected: FAIL, header not found" check;
  stubs are now .c files only.
- The CMakeLists listing omitted the USB common include the plan's own
  prose mandates, so the diagnostic console would silently not enumerate.
- The repo's docs/spec.md was still the pre-revision spec, contradicting
  the plan on three settled decisions. Refreshed.
- A preset recall replays CC 105 but never re-synced the cycle button's
  step index, so shimmer's LED lied, the next press jumped, and a later
  snapshot captured the stale step. button_engine_sync_cycle() added.
- Smaller: an unachievable "one message per millisecond" expectation, a
  boot-time flash dump a CDC console would swallow, four stale references
  to the old freeze pedal, an undocumented reboot desync, nRF52840 flash
  stalls versus the bit-banged MIDI ISR, and one test whose deadband
  rejection meant it never reached the branch its comment described.

Explicitly checked and found correct: all transplant line ranges, the
shimmer enum fix, prj.conf against the looper's working config, txqueue
coalescing and ring wrap, controls.c arithmetic and pickup, buttons.c and
presets.c bounds and state handling, and the freeze toggle decision itself.
