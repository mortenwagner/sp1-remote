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
MIDI-A input, driving the synth's existing CC map. A track button acts as the
freeze pedal (CC 64).

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

- **A (recommended): fork `sp1-tape-looper`.** It already initializes the
  board, reads controls, and transmits MIDI on the TRS jack. Strip the looper;
  keep board bring-up + MIDI TX; add fader-read → CC emission.
- **B (fallback): build on the `sp1-midi` Zephyr template** if the looper
  fork turns out tangled. More greenfield, same hardware facts.

**Control mapping (v1 default — all tunable, held in one table in firmware).**
Sorting rule (decided 2026-08-08): faders = continuous sweeps, buttons =
steps and moments. Not a mixer — the ADDAC120 already has per-pickup volume.

| Control | MIDI | Synth function |
|---|---|---|
| Fader 1 | CC 102 | filter cutoff |
| Fader 2 | CC 104 | reverb wet |
| Fader 3 | CC 107 | delay time (tape-glide warp) |
| Fader 4 | CC 108 | delay feedback |
| Track button 1 | CC 64 | freeze — tap = latch, hold = momentary pedal |
| Track button 2 | CC 105 | shimmer step (off/low/full/boost, LED shows state) |
| Track button 3 | CC burst | preset A — tap = replay, hold ~2 s = save |
| Track button 4 | CC burst | preset B — tap = replay, hold ~2 s = save |

**Unified button model (decided 2026-08-08):** every button is "send a list
of (CC, value) pairs." Freeze = a 1-item toggling list (CC 64). Shimmer = a
1-item list cycling 4 values (CC 105). Preset = a ~5-item list replaying
stored values on the EXISTING CCs (102/104/105/107/108) — no new CCs, no
synth-side work; the synth's existing fw CC smoothing turns a preset burst
into a glide/morph. Burst messages spaced ~1–2 ms to be gentle on the FIFO.

**Preset save = puck-local snapshot:** hold ~2 s stores the puck's last-sent
fader values + shimmer step into that button's slot (persisted in flash).
Caveat, accepted: the puck only knows its own view — tweaks made via Push 3
or the menu aren't captured (MIDI here is one-way). Freeze (CC 64) is
deliberately excluded from snapshots — it's a live gesture, not a scene.

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
CC emission: send-on-change only, 7-bit, rate-limited (~max one message per
10 ms per fader) so fader swipes don't flood the FIFO.

**LEDs:** fader LED trails mirror the last-sent value (the puck doubles as a
readout of the synth's four macro states). Play button LED = link heartbeat
(steady when powered, blink on CC send).

**Power:** internal battery; USB-C charging as normal. No firmware sleep work
in v1 beyond whatever the forked base already does.

## Pre-flight verifications (before any code)

1. SP-1 TRS MIDI pinout/polarity vs Tiliqua Type-A — bench-verify with the
   looper firmware's clock output into the Tiliqua before writing anything
   (this also proves the cable + FIFO end-to-end).
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
- Unknown: fader strips' absolute-position quality (they may be relative/
  gesture sensors). Resolve in pre-flight #2; if relative-only, v1 becomes
  pickup-mode (fader touch nudges value) — decide at bench, not in advance.

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
