# SP-1 Fader Remote — Design

**Date:** 2026-08-08 · **Status:** APPROVED 2026-08-08 (Morten, this session)
**Origin:** Device-fleet brainstorm (sticks/boxes/players), 2026-08-08
**Home:** own PUBLIC repo `mortenwagner/sp1-remote` (name adjustable at repo
creation), local clone `~/Documents/Other Creations/dev/sp1-remote` on Main
Mac (sibling of tiliqua/dodilab). Re-homed at approval: this is a **generic
4-fader/4-button TRS-MIDI controller**, not solely a string-synth accessory —
the PopGoblin string synth is the shipped default profile and first target.

## Goal

One Teenage Engineering SP-1 stem player, reflashed with custom firmware, acts
as a dedicated 4-fader performance controller for the PopGoblin string synth:
fader strips send MIDI CCs out the SP-1's TRS jack into the Tiliqua's TRS
MIDI-A input, driving the synth's existing CC map. A track button toggles
freeze (CC 64).

"Done" (v1): Morten performs one rack session riding cutoff / reverb-wet /
delay-time / delay-feedback from the puck, hands off Push 3 entirely for
those gestures, and it feels better than menu-diving.

## Non-goals (v1)

- No audio playback from the puck (stems/beds parked — decided 2026-08-08).
- No BLE, no USB MIDI, no multi-puck anything. One puck, one cable.
- No MIDI clock output (the looper firmware already proves it; add later as
  v1.1 if the delay clock-sync firmware task lands on the synth side).
- No changes to the string synth at all (gateware or firmware). The synth's
  existing CC map is the entire contract; presets are implemented puck-side
  as CC sequences (decided 2026-08-08 — see mapping section).

## Context (researched 2026-08-08)

- SP-1 = unreleased TE prototype, nRF52840 MCU, 4 touch fader strips with LED
  trails, 4 track buttons + play + function, USB-C, audio out jack **plus a
  second 3.5mm TRS jack** which community firmware uses for MIDI.
- Community ecosystem: solderless.io browser flasher (WebSerial, no soldering;
  bootloader recovery = hold Track1+Track4 at power-on), `chattock/
  sp1-tape-looper` (sends 24ppqn MIDI clock + start/stop out the TRS jack —
  the proven TRS MIDI TX code), `ericlewis/sp1-midi` (Zephyr board template),
  `softmodded/marisko` (community firmware + board def), `softmodded/spire`
  (Renode emulator). feldd.com proves USB/BLE MIDI out is feasible.
- Tiliqua side: TRS MIDI **Type A** input is the synth's universal control
  path (Push 3 uses it today). Live CC map in the popgoblin SoC: 102 cutoff,
  103 reso, 104 reverb-wet, 105 shimmer (4-step), 106 chorus rate, 107 delay
  time, 108 delay feedback, 64 freeze, 43 palette, 51 plot source.

## Design

**Signal path:** SP-1 TRS jack (UART MIDI, 31250 baud) → TRS cable (polarity
adapter if needed) → Tiliqua TRS-A in → existing MidiRead FIFO → CC map.

**Firmware approach — two candidates, pick after pre-flight:**

- **A (recommended, reframed after Codex review 2026-08-08): lean Zephyr
  base + driver transplant.** Start from the `sp1-midi` template / marisko
  board definition and TRANSPLANT the proven pieces from `sp1-tape-looper`
  (MIDI TX on TRS, fader read, LEDs, power/charging). Rationale: the looper
  has grown into a large firmware (100+ commits, audio/storage engine in one
  big main.c, custom Zephyr patch) — "strip the looper" risks becoming the
  project.
- **B (fallback): direct looper fork** if transplanting turns out harder
  than expected at pre-flight (e.g. board bring-up outside the looper is
  flaky).

**Control mapping (v1 default — all tunable, held in one table in firmware).**
Sorting rule (decided 2026-08-08): faders = continuous sweeps, buttons =
steps and moments. Not a mixer — the ADDAC120 already has per-pickup volume.

| Control | MIDI | Synth function |
|---|---|---|
| Fader 1 | CC 102 | filter cutoff |
| Fader 2 | CC 104 | reverb wet |
| Fader 3 | CC 107 | delay time (tape-glide warp) |
| Fader 4 | CC 108 | delay feedback |
| Track button 1 | CC 64 | freeze — toggle on press (revised 2026-08-08) |
| Track button 2 | CC 105 | shimmer step (off/low/full/boost, LED shows state) |
| Track button 3 | CC burst | preset A — tap = replay, hold ~2 s = save |
| Track button 4 | CC burst | preset B — tap = replay, hold ~2 s = save |

**Unified button model (decided 2026-08-08, refined after Codex review):**
every button has a TYPE + a list of (CC, value) pairs. Types: *toggle*
(freeze), *cycle* (shimmer, 4 values), *preset* (replay a stored ~5-item
list on the EXISTING CCs 102/104/105/107/108 — no new CCs, no synth-side
work; the synth's fw CC smoothing turns the burst into a glide/morph).
Hold-to-save applies ONLY to preset-type buttons; toggle and cycle buttons
have no hold semantics at all and act on the PRESS (revised 2026-08-08 with
the freeze toggle: nothing outside preset replay depends on a release
arriving). Burst messages spaced ~1–2 ms to be gentle on the FIFO.

**Freeze = plain toggle (REVISED 2026-08-08 by Morten, superseding the timed
state machine below).** CC 64 obeys sustain semantics (≥64 on, <64 off).
Press alternates 127 / 0. Release does nothing. Power-up sends nothing.

Why the reversal: the timed machine made *release* load-bearing, and a
release that is late, mis-decoded or never delivered strands freeze ON with
no way back — the worst failure on a live gesture. With an inert release
there is nothing to strand: a lost press costs one extra press, and a
dropped message re-syncs on the next press. It also deletes the momentary
threshold, the pedal timeout and all duration plumbing from the button
layer. Cost: the sub-second stab (two quick taps are harder to place than
press-and-lift). Parked as a per-button mode; the first session decides
whether it's missed.

~~Superseded: CC 64 sustain semantics. Press always sends 127. Release
before ~400 ms → stays latched. Release after ~400 ms → send 0 (momentary
pedal). Press while latched → send 0.~~

**Preset save = puck-local snapshot:** hold ~2 s stores the puck's last-sent
fader values + shimmer step into that button's slot (persisted in flash).
Caveat, accepted: the puck only knows its own view — tweaks made via Push 3
or the menu aren't captured (MIDI here is one-way). Freeze (CC 64) is
deliberately excluded from snapshots — it's a live gesture, not a scene.

**Takeover policy (added after Codex review 2026-08-08):** during a
performance the puck is the AUTHORITATIVE controller for its four CCs —
don't ride the same params from Push 3 concurrently. After a preset recall
(the one case where the puck itself desyncs faders from sent values),
faders enter soft pickup: a fader's output is suppressed until its physical
position crosses the recalled value, then it takes over. (Pickup =
cross-to-catch, not "touch nudges value.")

**Mapping configurability:** v1 = one config table in firmware defines the
whole surface — per-fader CC + channel, per-button CC list + channel (values
re-programmable from the instrument via hold-to-save). v1.1 (parked) =
browser-based editor over WebSerial exposing that same table: assign any CC,
any channel, value ranges, per control. That makes the firmware a **generic
4-fader/4-button MIDI controller** (decided 2026-08-08) with the string-synth
map merely the shipped default profile — usable later against norns, Ableton,
or any TRS-MIDI gear without reflashing. WebSerial viability proven on this
hardware by feldd's browser remapping and solderless's flasher. Palette
cycling (CC 43) lost its button; if the visuals gesture is missed, revisit as
a function-button shift combo (parked).

Channel: 1 (verify what the SoC listens on — likely omni; confirm at bench).
CC emission (tightened after Codex review): send-on-change with per-fader
deadband, all sends through ONE coalescing TX queue (latest value per CC
wins) on interrupt/async UART — never blocking polled TX. Rationale: four
faders naively rate-limited at 10 ms each ≈ 38% of the 31.25 kbaud wire;
coalescing keeps worst-case latency flat when all four move at once.

**LEDs:** fader LED trails mirror the last-sent value — i.e. the PUCK's own
state, which equals the synth's state only under the takeover policy above
(MIDI is one-way; there is no actual readback). Play button LED = power +
TX-activity blink — a local indicator, NOT a link heartbeat (an
unacknowledged one-way line can't prove the other end is listening).

**Power:** internal battery; USB-C charging as normal. No firmware sleep work
in v1 beyond whatever the forked base already does.

## Pre-flight verifications (before any code)

1. SP-1 TRS MIDI electrical contract vs Tiliqua Type-A (whose input is
   optoisolated — a current loop, not a logic-level UART line): pin order,
   idle polarity, source resistance/drive current, then bench-verify with
   the looper firmware's clock output into the Tiliqua before writing
   anything (this also proves the cable + FIFO end-to-end).
2. Locate fader-read code in `sp1-tape-looper` / `SP-1-dev` docs; confirm
   fader resolution and read API.
3. Flash + recovery drill on ONE puck via solderless.io before development
   (flash looper, recover to it, confirm bootloader entry works). The other
   pucks stay stock until v1 is proven.
4. Confirm the popgoblin SoC's MIDI channel behavior (omni vs fixed).

## Staging & stop rules

- **v0 (no custom code):** flash `sp1-tape-looper`, cable its MIDI clock into
  the Tiliqua, confirm bytes arrive (scope/FIFO). Proves the entire link.
- **v1:** the fader→CC firmware above. One rack session.
- **Stop rule:** if after one real session the puck doesn't beat Push 3 for
  performing these four gestures, park the firmware (it stays flashable) and
  return the puck to stock. The other 7 pucks are untouched either way.

## Risks

- SP-1s are unreleased prototypes — scarce hardware. Mitigation: develop on
  one puck only; recovery path verified before dev; `spire` emulator exists
  for firmware iteration without hardware.
- Touch-fader jitter → CC zipper on the synth. Mitigation: firmware
  hysteresis/smoothing; the synth already smooths CC sweeps (fw glide).
- Fader strips are absolute-position sensors (community firmware uses them
  as absolute volume/scrub controls — Codex verified 2026-08-08); the REAL
  unknowns are resolution, jitter, touch/release behavior, dead zones, and
  calibration. Measure at pre-flight #2 and size the deadband from data.

## Open questions (resolve during planning, not blockers)

- Hold-to-save threshold feel (~2 s) and how the LED confirms a save.
- Whether to add function-button-as-shift for a second CC bank / palette
  cycling (parked; YAGNI until v1 has been performed with).

## Next step

Spec approved 2026-08-08. Implementation happens in a dev session on Main
Mac: create the public repo + local clone, then `writing-plans` produces the
phased implementation plan (starting from the pre-flight verifications and
the zero-code v0). Decisions in this spec are settled — don't re-litigate
without new bench evidence.

## Implementation home (added 2026-08-08, after approval)

Repo created and plan written the same day. This file stays the frozen
design record; the repo owns everything from here.

- **Repo:** github.com/mortenwagner/sp1-remote (public), clone at
  `~/Documents/Other Creations/dev/sp1-remote`. Verbatim copy of this spec
  at `docs/spec.md`.
- **Plan:** `docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md`,
  9 phases, TDD, host-testable pure logic + hardware-verified lifts.
- **Three pre-flight corrections from source evidence** (design decisions
  untouched):
  1. popgoblin builds its serial decoder with `forward_rt=False`, so MIDI
     clock never reaches `midi_read`. The v0 in this spec (looper clock →
     popgoblin FIFO) would have shown nothing even with a perfect link.
     v0 is now a byte-level monitor leg, plus an optional polysyn leg
     (polysyn does forward real-time, onto audio channel 1).
  2. The looper's TRS MIDI TX is called untested in its own source, so the
     toolchain now precedes the bench: a polarity fix is a `MIDI_INVERT`
     flip plus a rebuild, not a lost session.
  3. Eight discrete LEDs exist, not per-fader trails, so fader value is
     rendered as LED brightness.
- **Answered from source, no bench needed:** the synth is omni (it parses
  `ControlChange(_, cc, val)`), and CC 64 is level-based (`v >= 64`), so
  tap-latch vs hold-momentary is entirely puck-side.
- **Still genuinely unknown:** the sync jack. It is absent from
  `SP-1-dev/src/stemplayer_pins.h` and its wiki page is a Todo, so the
  looper's pin constants come from an unpublished source. Measure before
  connecting anything to the Tiliqua.
- **Plan reconciled with this spec's same-day revision** (base reframe, TX
  queue, soft pickup, 400 ms freeze): all four implemented.
- **Codex red-teamed the plan** (gpt-5.6-sol xhigh; raw output kept at
  `sp1-remote/docs/codex-review-2026-08-08.md`). It confirmed the three
  source claims above and found 12 issues, all folded in. The two that
  mattered: the looper's `enter_dfu()` escape hatch (Track1+Track4 while
  running) was missing from the transplant list and several cited line
  ranges were wrong, which together would have shipped a firmware with no
  watchdog installed and no way back from a wedged app; and momentary
  freeze was unreachable, because the release cleared the press timer
  before the duration was read, so a long hold would have latched freeze
  instead of releasing it.
- **Shimmer values were inverted and are now fixed:** PopGoblin's
  `ShimmerLevel` iterates Boost, Full, Low, Off with `index = cc*4/128`,
  so the buckets are 0-31 Boost, 32-63 Full, 64-95 Low, 96-127 Off. The
  puck sends bucket centres and starts on Full, where the synth boots.
- **Rate ceiling is the synth, not the wire:** popgoblin pops one MIDI FIFO
  entry per 5 ms ISR, so 200 msg/s is the real limit. The TX queue paces
  to that.
