# Codex adversarial review, 2026-08-08

Raw output from `codex exec` (gpt-5.6-sol, reasoning effort xhigh) run against
this repo with the tiliqua and SP-1 reference sources readable. The brief was
to find problems, not to rewrite: hardware risk first, then factual errors in
the plan's source claims, then plan gaps, then C correctness.

Captured with `tail -120`, so the first few lines are the tail of the stream
and the review proper starts at "Do not execute this plan as written".

Verdict: the plan's three source claims about the Tiliqua were confirmed
correct. Twelve findings were returned. Every one of them is folded into
`docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md`; the ones that
changed behaviour are marked "REVIEW" at the point of change.

The two that mattered most:
- `enter_dfu()` (Track 1 + Track 4 while the app runs) was missing from the
  plan's transplant list, and several cited line ranges were wrong, which
  together would have produced a firmware with no watchdog installed, a
  power-off that never powers off, and no recovery path from a wedged app.
- The momentary-freeze integration read the press duration after the release
  had already cleared it, so a long hold would have latched freeze instead of
  releasing it. The unit tests passed because they never exercised that
  ordering. `test_release_reports_the_real_press_duration` now pins it.

---

- The unsigned `uint32_t(now - then)` time arithmetic handles the ~49.7-day millisecond wrap correctly for these short intervals.
- The 40-byte page loop has no off-by-one: it uses 102 records at offsets 0 through 4040 and leaves 16 unusable bytes.
- Corrupt-tail fallback works as intended.

One additional low-level bug: `button_engine_set_preset()` accepts negative `len`; casting it to `size_t` makes `memcpy` enormous ([plan:1955-1965](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:1955>)). Current internal callers should be non-negative, but the interface itself is unsafe.

No repository files were changed.
[2mtokens used[0m
276.180
Do not execute this plan as written. The three challenged Tiliqua claims are correct, but there are two credible device-recovery hazards and the freeze pedal implementation is broken.

## Ranked findings

1. **CRITICAL — Phase 7 erases an internal-flash page whose ownership is not proven.**

   The plan infers that `0xFF000` is free application storage because the flasher stops before it, then erases the entire page when full ([plan:52](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:52>), [plan:2537](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:2537>)). That does not prove the bootloader does not own it for settings, validation state, or recovery metadata.

   Both reference DTS files label it `storage`, but neither demonstrates actual writes on hardware or bootloader non-use ([sp1-midi DTS:424](</Users/morten/.claude/jobs/3ee28324/tmp/sp1-midi/boards/teenageengineering/stem_player/stem_player.dts:424>), [looper DTS:173](</Users/morten/.claude/jobs/3ee28324/tmp/sp1-tape-looper/boards/teenageengineering/stem_player/stem_player.dts:173>)). If the label is wrong, `flash_area_erase()` can invalidate boot/recovery state. This is precisely the kind of assumption that must be proved before destructive access, not by the persistence test afterward.

2. **CRITICAL — the “BIG FIVE” lift instructions point at the wrong source ranges and omit the source firmware’s recovery escape hatch.**

   The plan says to lift watchdog installation from lines `5524-5583` and `power_off()` from `5664-5720` ([plan:683-698](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:683>)). In the source:

   - Watchdog installation is actually at [main.c:5901](</Users/morten/.claude/jobs/3ee28324/tmp/sp1-tape-looper/firmware/src/main.c:5901>).
   - `RESETREAS` clearing and the actual `SYSTEMOFF` write are at [main.c:5730](</Users/morten/.claude/jobs/3ee28324/tmp/sp1-tape-looper/firmware/src/main.c:5730>), outside the stated range.
   - The looper also has an application-level Track1+Track4 reset-to-DFU path at [main.c:5737](</Users/morten/.claude/jobs/3ee28324/tmp/sp1-tape-looper/firmware/src/main.c:5737>) and [main.c:6964](</Users/morten/.claude/jobs/3ee28324/tmp/sp1-tape-looper/firmware/src/main.c:6964>); the plan drops it entirely.

   A healthy-app power-off drill does not test recovery from a wedged app. Following the quoted ranges literally can leave no installed watchdog, no working `SYSTEM_OFF`, and no forced DFU path—the exact soft-brick condition the plan itself warns about.

3. **HIGH — “no extra series resistor” is not adequately justified. Normal operation is probably safe for the Tiliqua; short-circuit safety is not.**

   The r5.1 schematic does show the input LED behind R10 = 220 Ω, with antiparallel D1 protection. :codex-file-citation{path="/Users/morten/Documents/Other Creations/dev/tiliqua/hardware/schematics/tiliqua-motherboard-r5.1.pdf" purpose="source"} At a verified 3.3 V source, roughly 8–10 mA is below the H11L1’s 30 mA maximum, so the Tiliqua input is unlikely to be damaged in the proposed normal connection. [Onsemi’s H11L1 datasheet](https://www.onsemi.com/download/data-sheet/pdf/h11l3m-d.pdf) confirms that limit.

   But the plan measures only unloaded voltage and then declares the receiver’s resistor sufficient ([plan:439-453](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:439>)). It has not verified:

   - SP-1 ring voltage under the 220 Ω plus LED load.
   - Existing SP-1 output resistance/current limiting.
   - Tolerance of a ring-to-sleeve short during TRS insertion or a faulty adapter.
   - Whether the PNP is fed from regulated 3.3 V or another rail.
   - The unpublished transistor topology beyond a source comment.

   The MIDI electrical specification explicitly requires transmitter-side short-circuit limiting; for 3.3 V it specifies transmitter resistors in addition to the receiver’s 220 Ω. [MIDI Association electrical specification](https://www.midi.org/wp-content/uploads/wpforo/default_attachments/1709416667-ca33-MIDI-10-Electrical-Specification-Update.pdf)

   If the polarity is wrong, the likely result is silence/garbage, not Tiliqua damage—D1 protects the optocoupler. If the output lacks short protection, the credible damage case is the scarce SP-1’s PNP/output rail during plug insertion or a wiring short.

4. **HIGH — momentary freeze can never work with the supplied integration.**

   On release, `btn_update()` clears `st->down` ([plan:1585-1591](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:1585>)). `btn_held_ms()` then returns zero whenever `down` is false ([plan:1597](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:1597>)). The main loop calls `btn_update()` first and `btn_held_ms()` afterward ([plan:2069-2076](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:2069>)).

   Therefore every release reaches the freeze engine with `held_ms == 0`; the `>= 300 ms` pedal branch is unreachable ([plan:1894-1905](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:1894>)). A one-second hold latches freeze instead of releasing it. The expected result at [plan:2093-2098](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:2093>) is impossible.

5. **HIGH — a genuinely missed RELEASE leaves freeze stuck indefinitely; debounce does not mitigate that.**

   The three-pass debounce only rejects short disagreement. ADC errors explicitly retain the committed pressed state ([plan:2016-2032](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:2016>)). There is no maximum pedal duration, timeout-generated OFF, reboot-time CC64 OFF, or link-loss recovery. If RELEASE never reaches the FSM, the synth retains CC64 = 127 indefinitely.

   The final risk statement claiming debounce and fast-tap testing mitigate an entirely absent release is false ([plan:2694](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:2694>)).

6. **HIGH — the shimmer table is semantically reversed and initially desynchronised from PopGoblin.**

   The plan sends `{0, 42, 85, 127}` ([plan:1374-1378](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:1374>)). PopGoblin’s enum iteration order is `Boost, Full, Low, Off`, with `Full` the default ([options.rs:27](</Users/morten/Documents/Other Creations/dev/tiliqua/gateware/src/top/popgoblin/fw/src/options.rs:27>)); CC values select enum quartiles in that order ([enumeration.rs:96](</Users/morten/Documents/Other Creations/dev/tiliqua/gateware/src/rs/opts/src/enumeration.rs:96>)).

   Consequences:

   - Value 0 means Boost, not Off.
   - The first tap sends 42 = Full, so it produces no change from the synth default.
   - The controller shows step 0/dark while the synth is Full.
   - Saving a preset before touching shimmer captures 0 = Boost, silently changing shimmer on recall.

7. **HIGH — the promised USB diagnostic console will not be initialized.**

   The plan selects the next USB stack while explicitly setting `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n` ([plan:601-610](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:601>)), but supplies no application USB initialization. The source firmware includes Zephyr’s USB common helper in CMake ([looper CMakeLists.txt:4](</Users/morten/.claude/jobs/3ee28324/tmp/sp1-tape-looper/firmware/CMakeLists.txt:4>)) and explicitly calls `sample_usbd_init_device()` and `usbd_enable()` ([main.c:4864](</Users/morten/.claude/jobs/3ee28324/tmp/sp1-tape-looper/firmware/src/main.c:4864>)). The plan does neither.

   Task 2.2’s expected `/dev/tty.usbmodem*` console ([plan:820-827](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:820>)) is therefore not observable as described.

8. **MEDIUM — the approved generic per-button list configuration is absent.**

   The frozen spec requires a config table containing a per-button CC list plus channel ([spec:90-97](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/spec.md:90>)). `button_cfg_t` contains one CC, scalar on/off values, or one four-value step array; it cannot express an arbitrary list of `(CC,value)` pairs ([plan:1327-1336](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:1327>)). The runtime engine emits lists, but the profile table cannot configure them. That is an uncovered approved requirement.

9. **MEDIUM — the preset snapshot can overflow its output buffer under a valid “generic” profile.**

   `n < out_max` is checked only in the outer capture loop. Neither inner loop checks before `out[n++]` ([plan:2575-2587](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:2575>)). Duplicate CC assignments across multiple faders or STEP buttons can write beyond the five-element snapshot buffer. The shipped default is safe; the claimed generic mapping is not.

   The subsequent instruction to “persist the whole bank” is also incomplete: `preset_store_save()` accepts `preset_bank_t`, but the plan only updates `button_engine_t` and gives no bank assembly step ([plan:2595](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:2595>)).

10. **MEDIUM — aggregate fader traffic can exceed PopGoblin’s drain rate.**

    The plan limits each fader independently to 100 messages/s, allowing a nominal aggregate of 400/s ([plan:56](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:56>)). PopGoblin has an eight-entry FIFO ([top.py:150](</Users/morten/Documents/Other Creations/dev/tiliqua/gateware/src/top/popgoblin/top.py:150>)) and firmware reads one entry per 5 ms ISR, at most 200/s ([main.rs:28](</Users/morten/Documents/Other Creations/dev/tiliqua/gateware/src/top/popgoblin/fw/src/main.rs:28>), [main.rs:100](</Users/morten/Documents/Other Creations/dev/tiliqua/gateware/src/top/popgoblin/fw/src/main.rs:100>)). Sustained multi-fader movement can overflow silently; if the final CC is dropped, the puck will not resend it once stationary.

11. **MEDIUM — two supplied build steps cannot pass as written.**

    - `midi_tx_init()` is declared and called, but Task 3.1 supplies no definition; it says the only new code is `midi_tx_cc()` ([plan:905-937](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:905>)).
    - The buttons test uses `profile_popgoblin_default` ([plan:1647-1650](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:1647>)), but `SRC_buttons` omits `profile.c` ([plan:336-340](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:336>)). It will fail at link time.

12. **LOW — several “expected” observations are not supported by the described setup.**

    - A meter may not show the tiny average-voltage change from sparse MIDI clock bytes; the scope path is valid, the meter path is not dependable ([plan:445-447](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:445>)).
    - “Battery gains charge over a few minutes” has no measurement method ([plan:702](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:702>)).
    - The shimmer LED API is boolean, but the hardware test expects four brightness levels ([plan:2132-2154](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:2132>)).
    - The cited “on-screen MIDI indicator” is actually motherboard LED handling ([ui.rs:115](</Users/morten/Documents/Other Creations/dev/tiliqua/gateware/src/rs/lib/src/ui.rs:115>)).

## The three challenged factual claims

None is wrong.

- **(a) Correct:** `MidiDecodeSerial()` defaults to `forward_rt=False`; `MidiRTFilter` consumes real-time bytes and only creates the sideband when forwarding is enabled ([decode_serial.py:46](</Users/morten/Documents/Other Creations/dev/tiliqua/gateware/src/tiliqua/midi/decode_serial.py:46>), [decode_serial.py:151](</Users/morten/Documents/Other Creations/dev/tiliqua/gateware/src/tiliqua/midi/decode_serial.py:151>)). PopGoblin instantiates the default and connects only `.o` to its FIFO ([top.py:247](</Users/morten/Documents/Other Creations/dev/tiliqua/gateware/src/top/popgoblin/top.py:247>)). The looper’s transmitter calls send only for FA, FC and F8 ([main.c:4531](</Users/morten/.claude/jobs/3ee28324/tmp/sp1-tape-looper/firmware/src/main.c:4531>)).

- **(b) Correct:** PopGoblin matches `ControlChange(_, cc, val)` and discards the channel ([main.rs:115](</Users/morten/Documents/Other Creations/dev/tiliqua/gateware/src/top/popgoblin/fw/src/main.rs:115>)).

- **(c) Correct:** CC64 assigns `cc64_held = v >= 64`; it is level-based, not a synth-side toggle ([main.rs:120](</Users/morten/Documents/Other Creations/dev/tiliqua/gateware/src/top/popgoblin/fw/src/main.rs:120>)).

## C checks that are fine

- `raw * 127` cannot overflow for the clamped 0–3700 input range.
- The unsigned `uint32_t(now - then)` time arithmetic handles the ~49.7-day millisecond wrap correctly for these short intervals.
- The 40-byte page loop has no off-by-one: it uses 102 records at offsets 0 through 4040 and leaves 16 unusable bytes.
- Corrupt-tail fallback works as intended.

One additional low-level bug: `button_engine_set_preset()` accepts negative `len`; casting it to `size_t` makes `memcpy` enormous ([plan:1955-1965](</Users/morten/Documents/Other Creations/dev/sp1-remote/docs/superpowers/plans/2026-08-08-sp1-remote-implementation.md:1955>)). Current internal callers should be non-negative, but the interface itself is unsafe.

No repository files were changed.
