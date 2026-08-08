# SP-1 Remote

Firmware that turns a Teenage Engineering SP-1 stem player into a generic
4-fader / 4-button MIDI controller. It sends MIDI CCs out the SP-1's TRS
sync jack, and also over USB MIDI, so the puck can drive hardware from the
jack and a computer from the cable.

The shipped default profile targets the PopGoblin string synth on a Tiliqua,
but nothing in the firmware is specific to it: the whole control surface is
one table in `firmware/src/profile.c`.

## Status

Planning complete, firmware not yet built. The pure logic (fader
conditioning, soft pickup, button behaviour, the transmit queue, preset
serialisation) is written and unit-tested on the host. Hardware bring-up
starts at Phase 1 of the plan.

- Plan: `docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md`
- Design: `docs/spec.md`
- Reviews: `docs/codex-review-2026-08-08.md`, `docs/fable-review-2026-08-08.md`

## Default profile

| Control | MIDI | PopGoblin function |
|---|---|---|
| Fader 1 | CC 102 | filter cutoff |
| Fader 2 | CC 104 | reverb wet |
| Fader 3 | CC 107 | delay time |
| Fader 4 | CC 108 | delay feedback |
| Track button 1 | CC 64 | freeze, toggles on press |
| Track button 2 | CC 105 | shimmer, steps off / low / full / boost |
| Track button 3 | CC burst | preset A: press to recall, hold 2 s to save |
| Track button 4 | CC burst | preset B: press to recall, hold 2 s to save |

To change the mapping, edit `firmware/src/profile.c`. Nothing else in the
firmware hard-codes a CC number.

## Running the tests

The decision logic is Zephyr-free C and runs on any machine:

```sh
make -C tests/host test
```

## Building the firmware

Needs Zephyr v4.3.1 and Zephyr SDK 0.17.4. See `docs/toolchain.md` once
Phase 0 of the plan is complete.

```sh
west build -p -b stem_player firmware -- -DBOARD_ROOT=$(pwd)
```

## Flashing

Put the SP-1 in bootloader mode: power off, hold Track 1 + Track 4, plug in
USB-C, release once the Track 1 LED lights. Then flash `build/zephyr/zephyr.bin`
with the firmware utility at solderless.engineering, using Chrome or Edge.

## Recovery

There is no reset pin on this hardware, so read this before flashing anything.

- The bootloader is entered by the Track 1 + Track 4 hold above, from power off.
- While this firmware is running, holding Track 1 + Track 4 for about 1.2 s
  resets straight into the bootloader. All four track LEDs light to confirm.
- The firmware feeds a 4 second watchdog and uses SYSTEM_OFF as its only
  power-down path, so a hang reboots rather than bricking.

## Credits

Built on the work of the SP-1 community: chattock and marcabisamra
(sp1-tape-looper, the TRS MIDI transmitter and most of the board bring-up),
timknapen (SP-1-dev, the pin map and wiki), ericlewis (sp1-midi, the board
definition and USB MIDI), and the Solderless team (the browser flasher that
makes any of this reversible).
