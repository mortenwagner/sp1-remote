# Codex adversarial review of the firmware, 2026-08-08

Second Codex pass, this time on the Zephyr code written during the build
session: board_io.c, midi_trs.c, midi_usb.c, midi_tx.c, presets_flash.c,
main.c and the board files. The pure-logic modules were excluded, having
already had two review passes and 48 host tests.

Verdict: "I would not sign off this build for scarce hardware."

## Fixed in response

- **The ownership scan ignored the last 16 bytes.** 102 records of 40 bytes
  cover 4080, but the erase covers all 4096, so foreign data living only in
  that tail would be declared safe and destroyed. The scan now accounts for
  every byte.
- **The watchdog install was invalid on Zephyr 4.3.1.** The nRF driver needs
  WDT_FLAG_RESET_SOC and rejects a zero flags field with -ENOTSUP; both
  return codes were discarded, so a rejected install was followed by setup
  on a channel that did not exist. Not a brick, because the bootloader's
  watchdog is the one actually running and direct RR writes feed it, but the
  comment claimed a fallback that did not exist.
- **A torn write could lock saving out permanently.** A reset mid-write
  leaves an undecodable record, which the safety gate then treats as foreign
  forever. Added preset_store_repair(), human-triggered only.
- **Flash writes could corrupt an in-flight MIDI byte.** A word write stalls
  the CPU ~41 us against a 32 us MIDI bit. midi_tx_idle() now drains the
  queue and lets the wire settle before any flash operation.
- **The power-off hold counted iterations, not time.** The loop's real period
  is work plus 5 ms, so a diagnostic pass or a flash write stretched the
  nominal 2.5 s. Now timed from uptime.
- **The transmit counters overclaimed.** They count attempts, are written
  and read without synchronisation, and ignore both sinks' results. The
  comments now say so; they exist to show which stage stopped advancing.

## Accepted, with the reasoning stated

**The ownership gate is not proof of ownership.** Correct, and unfixable in
software: showing the page holds nothing foreign now cannot show the
bootloader will not use it later, or that it expects the page to stay
erased. What makes the risk acceptable is that the flasher deliberately
preserves this page, a human inspects the dump before the first save, and
the worst case is bootloader state we can reflash around. The code says this
plainly rather than implying more than it delivers.

## Open, needs a bench check

**Power-off no longer powers down the external chips.** The looper
explicitly mutes the amp and headphone codec, powers down the eMMC rail and
disables the oscillator at power-off, because retained GPIO levels had
caused overnight battery drain and audio noise (reference main.c:5693-5716).
That section was dropped on the reasoning that this firmware never brings
those subsystems up. But their state is whatever the bootloader left, and
SYSTEM_OFF retains GPIO levels.

Not a damage risk, a battery-drain risk. The check is cheap: power pop off
with a known battery reading, leave it overnight, read again. If it has
drained, transplant that section.

## Confirmed correct

Partition layout with no overlap; RESETREAS latched and cleared at boot and
before SYSTEM_OFF; every looping path feeds the watchdog, and the paths that
deliberately do not are after SYSTEM_OFF and reset, where a watchdog reset
is the desired outcome; DFU ordering, band check and GPREGRET magic; no
lost-update race on the LED shadow masks (one thread writer, ISR reads
only); the TIMER2 semaphore handshake, which forces MARK before releasing
and cannot deadlock the drain thread; page_buf in BSS rather than on a
stack; word-aligned flash offsets; and the ADC configuration and button
thresholds.
