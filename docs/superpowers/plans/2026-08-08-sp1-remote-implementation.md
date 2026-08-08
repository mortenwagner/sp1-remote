# SP-1 Remote Implementation Plan

> **Progress, 2026-08-08.** Phases 0 to 6 are implemented and verified on
> hardware; Phase 7 is implemented but not yet exercised. Measured results
> and the constants they corrected are in `docs/hardware-notes.md`. Task 1.2
> and 1.3 (the TRS electrical work) are deliberately deferred: with the USB
> MIDI sink in place, nothing else depends on them.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn one Teenage Engineering SP-1 into a generic 4-fader / 4-button TRS-MIDI controller whose shipped default profile drives the PopGoblin string synth's existing CC map.

**Architecture:** A small Zephyr application for the nRF52840 on a lean base (the `sp1-midi` board template), into which the proven hardware routines from `sp1-tape-looper` are transplanted: the bit-banged TRS MIDI transmitter, the ADC ladder reads, the LED renderer, and the watchdog and power path. All decision logic (fader conditioning, soft pickup, the button state machines, the coalescing transmit queue, the profile table, preset serialisation) lives in Zephyr-free C files that compile unchanged into a host test binary, so the logic is unit-tested on the Mac and only the electrical behaviour needs the bench.

**Tech Stack:** Zephyr RTOS v4.3.1, Zephyr SDK 0.17.4, CMake/Ninja/west, C11, host tests in plain C11 with clang, flashing via the local `solderless.engineering` mirror over WebSerial in Chrome.

**Adversarial review, twice.** This plan was red-teamed against the tiliqua and SP-1 sources on 2026-08-08, first by Codex (gpt-5.6-sol, xhigh) and then independently by Fable 5 reviewing the post-Codex version. Codex confirmed the three source claims below and returned twelve findings: two device-recovery hazards, four outright bugs, one uncovered spec requirement. Fable then verified every transplant citation against the looper source (all correct after the Codex round) and found five more, including one that Codex had left half-fixed and that mattered most of all: the documented Track 1 + Track 4 recovery combo falls inside this firmware's track-4 ADC band, so without an explicit pre-decode check it reads as a preset-save and destroys a stored scene instead of entering DFU. See Task 5.2 Step 1.

All findings from both passes are folded in. Points where the reasoning would otherwise be lost are marked "REVIEW" at the site of the change.

**Spec version this plan targets:** the 2026-08-08 design as revised the same day after the Codex 5.5 review and 5.6-sol verification (ClaudeLife commit `fd63e0d`). That revision reframed the firmware base, added a takeover policy, replaced per-fader rate limiting with a coalescing transmit queue, and pinned the freeze timing state machine. All four are implemented below.

---

## Findings that change the spec's pre-flight (read before starting)

Three facts came out of reading the reference sources. None touches an approved design decision; two change *how* v0 is verified, one narrows an assumption.

**1. MIDI clock cannot reach popgoblin's `MidiRead` FIFO. The spec's v0 as literally written would produce a false negative.**
`gateware/src/top/popgoblin/top.py:252` instantiates `midi.MidiDecodeSerial()` with the default `forward_rt=False`. In `gateware/src/tiliqua/midi/decode_serial.py:49-66`, System Real-Time bytes (0xF8 clock, 0xFA start, 0xFC stop) are pulled out of the byte stream by `RealTimeExtract`, and with `forward=False` they are dropped rather than exposed. Only channel-voice messages reach `pg_periph.i_midi`, which is what `midi_read()` reads. The `sp1-tape-looper` firmware sends *only* 0xF8/0xFA/0xFC. Pointing the looper at the popgoblin bitstream therefore shows nothing on screen even if the cable, polarity and baud rate are all perfect.

Consequence: v0 splits into two legs, both cheap.
- **v0a (mandatory):** SP-1 clock into a known-good MIDI input on the Mac, watched with a byte-level monitor. This proves baud, framing, polarity and the adapter wiring, and is the leg that actually de-risks everything.
- **v0b (optional, zero-code end-to-end):** the `polysyn` bitstream *does* forward real-time (`gateware/src/top/polysyn/top.py:600` uses `MidiDecodeSerial(forward_rt=True)`) and turns the clock into a square wave on audio output channel 1 (`top.py:637-639`). Flash polysyn, feed the looper's clock in, and the clock appears as a measurable pulse train out of the Tiliqua. If a polysyn r5 bitstream is not to hand and building one is a detour, skip v0b: the Tiliqua leg gets proven for real in Phase 3 by the first CC-emitting firmware, where popgoblin's on-screen MIDI-activity indicator and a moving cutoff value are unambiguous.

**2. The looper's own source calls its TRS MIDI TX unverified.**
`firmware/src/main.c:4417-4418`: "NOTE: untested on real gear yet, verify on a MIDI/PO device; if MIDI is silent/garbled, try flipping MIDI_INVERT." The changelog later measures clock drift ("~32 ms/min", CHANGELOG.md:64,155), which implies someone did observe it, but the code comment is what we have in writing. Treat the TX path as plausible, not proven. `MIDI_INVERT` (main.c:4422) is the first debug lever, and flipping it needs a rebuild, which is why the toolchain is stood up in Phase 0 *before* the bench session rather than after it.

The sync jack is also the one part of this hardware that the community has **not** published. `timknapen/SP-1-dev` documents the pinout thoroughly, and every pin this plan depends on for faders, LEDs, ladders and the charger matches the looper's map exactly (`src/stemplayer_pins.h:19-84`), which is welcome independent corroboration of the lifted constants. But `P0.23` (the BC807 base), `P0.20` and `P0.17` do not appear in that header at all, and the wiki's "MIDI / PO sync" page is marked *Todo* (`Peripherals.md:26-27`, `_Sidebar.md:16`). The looper's sync-jack constants therefore come from a source that is not in any repo, most likely the SP-1 dev Discord. Treat them as good but unpublished, which is precisely why Task 1.2 measures the jack instead of trusting the comment.

Electrical facts for the adapter: MIDI data leaves on the **ring** of the sync jack, driven by P0.23 (BC807 base) through a PNP that inverts (main.c:4405-4410). The tip carries Pocket-Operator pulses (P0.20 / P0.17). The Tiliqua input is a standard opto-isolated TRS-A stage: `H11L1SR2M` with 220R in series (hardware/schematics/tiliqua-motherboard-r5.1.pdf, sheet `midi`). At 3.3 V through 220R that is roughly 8 to 9 mA into the opto LED, comfortably inside its rating, so no extra series resistor is needed *if* the SP-1 sources 3.3 V through a low impedance. The adapter is therefore SP-1 ring to Tiliqua ring, SP-1 sleeve to Tiliqua tip, SP-1 tip unconnected. This is a hypothesis to confirm with a multimeter in Task 1.2, not a fact.

**3. The synth is omni, and CC 64 is level-based, not a toggle.**
`gateware/src/top/popgoblin/fw/src/main.rs:115-124` parses `MidiMessage::ControlChange(_, cc, val)` and discards the channel, so any channel works (pre-flight #4 answered from source). CC 64 sets `cc64_held = v >= 64`, i.e. the synth holds freeze for as long as the last value it saw was 64 or more. The spec's freeze timing state machine is therefore implemented entirely puck-side, which is what Phase 5 does.

**Firmware base: the spec's reframed option A, "lean base plus transplant".**
The revised spec settles this: start from the `sp1-midi` template and marisko board definition, and transplant the proven pieces from `sp1-tape-looper` (MIDI TX on TRS, fader read, LEDs, power and charging), because "strip the looper" risks becoming the project. Having read both, that is the right call and this plan follows it. Supporting evidence: `firmware/src/main.c` is 7614 lines of which roughly nine tenths is the looper engine, the eMMC driver and the I2S audio path, all interlocked through shared volatile globals. `sp1-midi` by contrast is a purpose-built BSP ("Fork this repo to build new firmware... MIDI controllers") with a watchdog, reset breadcrumbs, a charger driver and a fader-to-CC controller already in it (`app/MidiController.hpp:14-26`).

Two things the transplant must carry across, because `sp1-midi` cannot supply them:
- Its `MidiController` sends over **USB MIDI 2** (`usbd_midi2.h`), not the TRS jack. The bit-banged TRS transmitter exists only in the looper.
- Its board files declare PWM LED nodes. The looper's board file deliberately drops them, because PWM owning those pins fights the direct GPIO writes its soft-PWM renderer needs (`boards/.../stem_player.dts:15-17`). Apply the same two deltas, and cite that comment as the reason.

Fallback if the transplant stalls at board bring-up: a direct looper fork with the engine deleted (the spec's option B). Decide that at Task 2.1, on evidence, not in advance.

**One scope correction:** the spec says "fader LED trails mirror the last-sent value". The known pin map has eight discrete LEDs, not per-fader trails: four centre-row LEDs (main.c:101-104) and four track LEDs above the buttons (main.c:107-110). There is no evidence of an addressable trail. Phase 6 therefore renders fader value as *brightness* on the centre-row LED via the soft-PWM renderer, and uses the track LEDs for button state. Task 2.3 includes a short LED survey to confirm there is nothing else on the panel; if a trail turns up, Phase 6 grows a task.

---

## Global Constraints

- **Develop on ONE puck only, named "pop"** (designated 2026-08-08). The other pucks stay stock until v1 is proven. Every instruction below that says "the dev puck" means pop.
- **The SP-1 "BIG FIVE" bootloader rules are non-negotiable** (source: `sp1-tape-looper/firmware/src/main.c:40-44`). The app lives at `0x20000`; the watchdog is fed at least every 5 s; bootloader-owned clocks and peripherals are not re-initialised; `SYSTEM_OFF` is the only power-down path; `RESETREAS` is cleared at boot and again before `SYSTEM_OFF`. There is no hardware reset pin on the SP-1. A firmware that hangs without feeding the watchdog, or that cannot get back to the bootloader, is a brick.
- **Bootloader entry:** power off, hold Track 1 + Track 4, plug in USB-C, release once the Track 1 LED lights.
- **No button behaviour depends on a release arriving.** Toggle and cycle act on the press; preset acts on the hold (which fires while the button is still down) or on the tap. Only the preset replay waits for a release, and a lost one there costs a replay, not a stuck parameter.
- **The firmware must carry its own escape hatch.** REVIEW: the looper does not rely on the power-off path alone. It implements `enter_dfu()` (`main.c:5743-5752`): holding Track 1 + Track 4 for 1.2 s *while the app is running* writes `GPREGRET = 0x57` and calls `NVIC_SystemReset()`, so the bootloader's own button scan catches the still-held combo and enters DFU. It is triggered from the control loop at `main.c:6981`. This is the difference between "recoverable" and "recoverable only while the app is healthy", and Task 2.1 transplants it. A power-off drill on a healthy app does not test recovery from a wedged one.
- **Transmit spacing is set by the receiver, not by us.** REVIEW: popgoblin's MIDI FIFO is 8 entries deep (`top.py:150`) and its firmware drains exactly one entry per 5 ms timer ISR (`main.rs:28,100`), so the synth absorbs at most 200 messages per second. The drain thread therefore paces messages at 5 ms, which is also why coalescing matters: it is the queue, not the wire, that must absorb a fast sweep.
- **Flash image format:** raw `.bin`, written to `0x20000`, maximum size `0xDF000` (`solderless/utility/js/protocol.js:8-13`). The flasher never touches `0xFF000` and above, so the 4 KB storage partition survives a reflash. Preset data must live there and nowhere else.
- **Only one 4 KB page of storage exists** (`0xFF000` to `0x100000`). Zephyr NVS needs two sectors, so it is not usable. Presets use an append-log inside the single page (Phase 7).
- **Do not erase that page until its ownership is proved.** REVIEW: both reference board files *label* `0xFF000` as `storage`, but a label is not evidence that the TE bootloader does not keep settings or recovery metadata there. Erasing it on a hunch is a plausible soft-brick. Task 7.0 dumps and inspects the page before anything writes to it, and the append-log is designed so that the erase only happens after 102 saves, by which point the page is demonstrably ours.
- **Two transmit sinks, one queue.** Every message goes to the TRS jack (the delivery path, and the only one popgoblin can hear) and to USB MIDI (a development instrument, and later a way to drive a Mac). ADDED 2026-08-08, overriding the spec's "no USB MIDI" non-goal, because it decouples all firmware behaviour work from the single undocumented part of this hardware. Note what it does NOT do: `popgoblin/top.py:249-253` instantiates only the TRS serial receiver, with no USB MIDI host, so USB cannot drive the string synth. Only `polysyn` has a USB MIDI host.
- **No changes to the synth.** Not gateware, not firmware. The popgoblin CC map is the entire contract.
- **Fader full-scale raw code is 3700** on the 12-bit SAADC as configured (gain 1/6, 0.6 V internal reference, 20 us acquisition). Source: `main.c:7487`. Confirm on this unit in Task 2.2 and size the deadband from measured jitter, per the spec's revised risk note.
- **Transmit path:** send-on-change with a per-fader deadband, and **every** message goes through ONE coalescing transmit queue where the latest value for a given (channel, CC) wins. No per-fader millisecond rate limit: four faders naively limited to one message per 10 ms each would occupy about 38 percent of the 31250 baud wire, and coalescing keeps worst-case latency flat when all four move together. The control loop must never block on transmission.
- **The puck is silent at power-on.** It has no readback: MIDI here is one-way. The first ADC reading of each fader only seeds state. Nothing is transmitted until a control is actually moved or pressed.
- **Takeover policy:** during a performance the puck is the authoritative controller for its four CCs. After a preset recall (the one case where the puck's own faders desync from what it just sent), each affected fader enters soft pickup: its output is suppressed until its physical position crosses the recalled value, then it takes over. Cross-to-catch, not touch-to-jump.
- **Freeze is a plain toggle (CC 64, sustain semantics, at least 64 is on).** Press alternates 127 and 0. Release does nothing at all. DECIDED 2026-08-08, replacing the spec's timed tap-versus-hold machine, on the grounds that release was load-bearing there: a release that was late, mis-decoded or never delivered stranded freeze in the on state with no way back, which is the worst possible failure on a live gesture. With an inert release, a lost press costs one extra press and a dropped message re-syncs on the next one. The cost is the sub-second stab, where two quick taps are harder to place than a press-and-lift; that is parked, not lost, and the first session decides whether it is missed. This also removes the momentary threshold, the pedal timeout, and all duration plumbing from the button layer.
- **Default MIDI channel is 1** (wire value 0). Per-control channel is configurable in the profile table.
- **Zephyr v4.3.1 with SDK 0.17.4.** Pinned. Installed and verified 2026-08-08; see `docs/toolchain.md`.
- **The firmware source tree must live at a path with NO SPACES.** Zephyr's devicetree preprocessing splits the overlay list on whitespace, so `~/Documents/Other Creations/...` fails with `fatal error: /Users/morten/Documents/Other: No such file or directory`. A symlink does not help: west and CMake resolve it back to the physical path. Host unit tests are unaffected (plain clang), so only the firmware build cares. This is a Zephyr limitation, not something to work around.
- **`ZEPHYR_TOOLCHAIN_VARIANT=zephyr` must be exported**, or the build dies in `FindZephyr-sdk.cmake:57` on an unquoted variable expansion, with a CMake syntax error rather than a useful message.
- **Licence:** MIT, matching the upstream code being transplanted. Every transplanted block keeps an attribution comment naming the source file and line range.
- **No em-dashes in prose written into this repo** (README, docs, commit messages). Use colons, commas, parentheses.

---

## File Structure

```
sp1-remote/
  README.md                       what it is, how to flash, how to recover
  LICENSE                         MIT
  .gitignore                      build/, refs/, *.bin artefacts
  docs/
    spec.md                       frozen copy of the approved design
    superpowers/plans/            this plan
    hardware-notes.md             bench-verified electrical facts (grows in Phase 1)
    flashing.md                   flash + recovery drill, written from real steps
    toolchain.md                  the exact commands that built it
  refs/
    fetch.sh                      clones the reference repos (gitignored contents)
  boards/teenageengineering/stem_player/
                                  from sp1-midi, with the looper's two deltas
  firmware/
    CMakeLists.txt
    prj.conf
    app.overlay                   ADC channels for the 4 faders + battery
    src/
      main.c                      init + the control loop, nothing else
      board_io.c / board_io.h     ADC ladders, button decode, LEDs, WDT, power
      midi_tx.c  / midi_tx.h      the drain thread + fan-out to both sinks
      midi_trs.c / midi_trs.h     bit-banged TRS MIDI TX (the delivery path)
      midi_usb.c / midi_usb.h     USB MIDI 2 sink (development + Mac use)
      txqueue.c  / txqueue.h      PURE: the coalescing transmit queue
      cc_msg.h                    PURE: the one shared message type
      profile.c  / profile.h      PURE: the config table + shipped default
      controls.c / controls.h     PURE: fader conditioning, pickup, button FSM
      buttons.c  / buttons.h      PURE: the unified button behaviour model
      presets.c  / presets.h      PURE: preset record encode/decode/page scan
      presets_flash.c             Zephyr flash IO for the storage page
  tests/host/
    Makefile                      builds and runs every pure-logic test with clang
    test_util.h                   tiny assert runner
    test_controls.c
    test_txqueue.c
    test_buttons.c
    test_presets.c
    test_profile.c
```

`controls.c`, `buttons.c`, `presets.c`, `profile.c`, `txqueue.c` and `cc_msg.h` must not include any Zephyr header. That is what makes the host tests possible, and it is the single most important structural rule in this plan.

**Every pure-logic implementation and every test in this plan was compiled and run before the plan was committed**, with `clang -std=c11 -Wall -Wextra -Werror`: 48 tests across five suites, all passing, including regression tests for every bug the two adversarial reviews found: the lost-release case the toggle design survives by construction, pickup landing exactly on its target, the duplicate-value suppression branch, and a preset recall re-syncing the cycle step. A failure when you run them means a transcription slip, not a design problem.

---

# Phase 0: Repository and toolchain

Nothing here touches hardware. It exists before the bench session on purpose: the most likely bench failure (MIDI polarity) is fixed by flipping a compile-time flag and rebuilding, and standing in front of the rack without a working build is the expensive version of that.

### Task 0.1: Repository skeleton

**Files:**
- Create: `README.md`
- Create: `LICENSE`
- Create: `.gitignore`
- Create: `refs/fetch.sh`

**Interfaces:**
- Consumes: nothing.
- Produces: `refs/fetch.sh` placing `refs/sp1-tape-looper`, `refs/sp1-midi` and `refs/SP-1-dev` on disk. Every later task that says "transplant from main.c:NNNN" reads from `refs/sp1-tape-looper/firmware/src/main.c`.

- [ ] **Step 1: Write `LICENSE`**

MIT, copyright "2026 Morten Wagner". Add a second paragraph below the licence text:

```
Portions of firmware/src (board bring-up, ADC ladder reads, LED soft-PWM,
watchdog and power handling, and the bit-banged TRS MIDI transmitter) are
derived from chattock/sp1-tape-looper, MIT licensed, and from work by
timknapen (SP-1-dev pin map) and ericlewis (sp1-midi board definition and
BSP, on which the board files here are based).
```

- [ ] **Step 2: Write `.gitignore`**

```gitignore
build/
refs/*
!refs/fetch.sh
*.bin
!firmware/release/*.bin
.DS_Store
tests/host/build/
```

- [ ] **Step 3: Write `refs/fetch.sh`**

```bash
#!/usr/bin/env bash
# Clone the SP-1 reference firmwares next to this script. Contents are
# gitignored: they are read-only references for transplanted code and the
# line numbers this plan cites.
set -euo pipefail
cd "$(dirname "$0")"
clone() {
  local url=$1 dir=$2
  if [ -d "$dir/.git" ]; then git -C "$dir" pull --ff-only; else git clone "$url" "$dir"; fi
}
clone https://github.com/chattock/sp1-tape-looper.git sp1-tape-looper
clone https://github.com/ericlewis/sp1-midi.git       sp1-midi
clone https://github.com/timknapen/SP-1-dev.git       SP-1-dev
clone https://github.com/timknapen/SP-1-dev.wiki.git  SP-1-dev-wiki
echo "references ready in $(pwd)"
```

The wiki is a separate repo from the code and holds the hardware pages. Note while you are there that its MIDI / PO sync page is a Todo: the sync jack is undocumented, which Task 1.2 exists to fix.

- [ ] **Step 4: Write `README.md`**

Sections, in this order: what the device is (one paragraph), the default profile table copied from the spec, how to flash (point at `docs/flashing.md`), how to recover a puck that will not boot, how to build, and a "status" line saying which phase of the plan is complete. Keep it under 80 lines. No em-dashes.

- [ ] **Step 5: Run the fetch script and verify**

```bash
chmod +x refs/fetch.sh && ./refs/fetch.sh
test -f refs/sp1-tape-looper/firmware/src/main.c && echo REFS_OK
```
Expected: `REFS_OK`.

- [ ] **Step 6: Commit**

```bash
git add README.md LICENSE .gitignore refs/fetch.sh docs/
git commit -m "chore: repository skeleton, licence, reference fetch script"
```

---

### Task 0.2: Zephyr toolchain, proven by building the looper unmodified

Building the looper is the toolchain proof *and* produces the v0 image Phase 1 flashes. It is not the base we develop on.

**Files:**
- Create: `docs/toolchain.md`

**Interfaces:**
- Consumes: `refs/sp1-tape-looper`.
- Produces: a working `west` in `~/zephyrproject`, and `refs/sp1-tape-looper/build/zephyr/zephyr.bin`.

- [ ] **Step 1: Install host dependencies**

```bash
brew install cmake ninja gperf python3 ccache dtc libmagic wget
cmake --version && ninja --version && dtc --version
```
Expected: CMake 3.20 or newer. None of these are currently installed on this Mac, so expect a real install, not a no-op.

- [ ] **Step 2: Create the west workspace on Zephyr v4.3.1**

```bash
python3 -m venv ~/zephyrproject/.venv
source ~/zephyrproject/.venv/bin/activate
pip install west
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.3.1 ~/zephyrproject
cd ~/zephyrproject && west update && west zephyr-export
pip install -r ~/zephyrproject/zephyr/scripts/requirements.txt
```

- [ ] **Step 3: Install Zephyr SDK 0.17.4**

```bash
cd ~ && wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.4/zephyr-sdk-0.17.4_macos-aarch64.tar.xz
tar xf zephyr-sdk-0.17.4_macos-aarch64.tar.xz && cd zephyr-sdk-0.17.4 && ./setup.sh -t arm-zephyr-eabi -h -c
```
On an Intel Mac use the `macos-x86_64` asset instead.

- [ ] **Step 4: Apply the looper's Zephyr patch**

Required only for this toolchain-proof build, because the looper uses USB audio. Our own firmware never enables UAC2 and does not need it.

```bash
cd ~/zephyrproject/zephyr
git apply /path/to/sp1-remote/refs/sp1-tape-looper/zephyr-patches/uac2-windows-fs-feedback.patch
git status --short | head
```
Expected: modified files under `subsys/usb`.

- [ ] **Step 5: Build the looper unmodified**

DONE 2026-08-08. The result was **byte-identical** to the repo's shipped `sp1_looper.bin` (md5 `d926854d751236e0ac21445828c7ed39`), which is a stronger proof than the size comparison this step originally asked for. Consequence: **Task 1.1 does not need a self-built image.** Flash the shipped `refs/sp1-tape-looper/sp1_looper.bin`; it is exactly what this toolchain produces.

Kept below for reproduction.


```bash
source ~/zephyrproject/.venv/bin/activate
export ZEPHYR_BASE=~/zephyrproject/zephyr
cd /path/to/sp1-remote/refs/sp1-tape-looper
west build -p -b stem_player firmware -- -DBOARD_ROOT=$(pwd)
ls -l build/zephyr/zephyr.bin
```
Expected: a `zephyr.bin` within a few percent of the size of the repo's shipped `sp1_looper.bin`. They will not be byte-identical (different toolchain build ids); a wildly different size means the build picked up the wrong board or config, so stop and fix before flashing anything.

- [ ] **Step 6: Write `docs/toolchain.md`**

Record the exact commands that worked, the SDK path, the venv path, and the two environment variables needed in a fresh shell (`ZEPHYR_BASE`, and sourcing the venv). Future sessions should not have to rediscover this.

- [ ] **Step 7: Commit**

```bash
git add docs/toolchain.md
git commit -m "docs: Zephyr v4.3.1 + SDK 0.17.4 toolchain, verified by building the looper"
```

---

### Task 0.3: Host test harness

**Files:**
- Create: `tests/host/Makefile`
- Create: `tests/host/test_util.h`
- Create: `tests/host/test_smoke.c`

**Interfaces:**
- Produces: `make -C tests/host` compiles every `test_*.c` against the pure sources in `firmware/src` and runs each binary. Later phases add one test file per pure module and one line to `SRC_<module>` in the Makefile.

- [ ] **Step 1: Write `tests/host/test_util.h`**

```c
#ifndef SP1_TEST_UTIL_H
#define SP1_TEST_UTIL_H
#include <stdio.h>
#include <stdlib.h>

static int sp1_test_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            sp1_test_failures++;                                             \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        long _a = (long)(actual), _e = (long)(expected);                     \
        if (_a != _e) {                                                      \
            printf("  FAIL %s:%d  %s: got %ld, want %ld\n",                  \
                   __FILE__, __LINE__, #actual, _a, _e);                     \
            sp1_test_failures++;                                             \
        }                                                                    \
    } while (0)

#define RUN(fn)                                                              \
    do { printf("- %s\n", #fn); fn(); } while (0)

#define TEST_MAIN_END()                                                      \
    do {                                                                     \
        if (sp1_test_failures) {                                             \
            printf("%d failure(s)\n", sp1_test_failures);                    \
            return 1;                                                        \
        }                                                                    \
        printf("ok\n");                                                      \
        return 0;                                                            \
    } while (0)

#endif
```

- [ ] **Step 2: Write `tests/host/test_smoke.c`**

```c
#include "test_util.h"

static void test_harness_reports_pass(void)
{
    CHECK_EQ(1 + 1, 2);
}

int main(void)
{
    RUN(test_harness_reports_pass);
    TEST_MAIN_END();
}
```

- [ ] **Step 3: Write `tests/host/Makefile`**

```make
# Host unit tests for the Zephyr-free logic modules. Each test binary links
# only the pure sources it needs; nothing here may pull in a Zephyr header.
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -g -I../../firmware/src -I.
BUILD   := build

SRC_smoke    :=
SRC_controls := ../../firmware/src/controls.c
SRC_txqueue  := ../../firmware/src/txqueue.c
SRC_profile  := ../../firmware/src/profile.c
SRC_buttons  := ../../firmware/src/buttons.c ../../firmware/src/controls.c \
                ../../firmware/src/profile.c
SRC_presets  := ../../firmware/src/presets.c ../../firmware/src/buttons.c \
                ../../firmware/src/controls.c ../../firmware/src/profile.c

TESTS := smoke

.PHONY: all test clean
all: test

test: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $(TESTS); do echo "== $$t"; $(BUILD)/$$t || exit 1; done

$(BUILD)/%: test_%.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(SRC_$*)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
```

- [ ] **Step 4: Run it**

```bash
make -C tests/host test
```
Expected: `== smoke`, `- test_harness_reports_pass`, `ok`.

- [ ] **Step 5: Commit**

```bash
git add tests/host
git commit -m "test: host unit-test harness for the Zephyr-free logic modules"
```

---

# Phase 1: Bench pre-flights

Hardware from here on. Stop rules are absolute: if Task 1.1 fails, the project stops until recovery works, because everything after it assumes a puck can be un-bricked.

**Ordering, revised 2026-08-08.** Only Task 1.1 is a hard gate. The TRS electrical work (1.2, 1.3, 1.4) needs a multimeter and a hand-made adapter, and neither is required to make progress: because the firmware now has a USB MIDI sink (Task 3.3), the entire control surface can be built and validated over a USB-C cable with no knowledge of the sync jack at all.

So the practical order when the meter and iron are not to hand is:

1. **Task 1.1** (recovery drill) — the gate, and it needs nothing but the puck, a cable and Chrome.
2. **Phase 2** (board bring-up) then **Task 3.2 + 3.3** (queue and USB MIDI). At that point pop is a working MIDI controller on the desk.
3. **Phases 4 to 7** developed and tested entirely over USB.
4. **Tasks 1.2, 1.3, 3.1** (the TRS path) whenever the meter and a way to make the adapter are available. Nothing above depends on them.

The only thing this ordering defers is driving the actual string synth, since popgoblin has no USB MIDI host. Everything else gets built and proven first.

### Task 1.1: Flash and recovery drill on the dev puck — DONE 2026-08-08, PASSED

Executed on pop. Results and the two gotchas that look like failure are in `docs/flashing.md`. Summary: bootloader entry from stock, looper flashed, and bootloader re-entry from running custom firmware, all confirmed. The second flash-and-re-enter cycle below was skipped deliberately: flashing always happens in bootloader mode, where the bootloader erases and rewrites the app slot, so whether the previous occupant was stock or custom does not change the operation. Steps kept below for the next puck.


**Files:**
- Create: `docs/flashing.md`

**Interfaces:**
- Produces: a dev puck running the looper firmware, and a written recovery procedure that has actually been executed twice.

- [ ] **Step 1: Confirm the dev puck**

The dev puck is **pop**. Label it if it is not already obvious which one it is, and record any serial or distinguishing mark in `docs/flashing.md`. The other pucks do not get connected during this project.

Needed for this task: pop, a USB-C cable, and Chrome. No multimeter, no adapter, no rack.

- [ ] **Step 2: Serve the local flasher**

```bash
cd "/Users/morten/Documents/Other Creations/dev/solderless/solderless-2026-05-18"
python3 -m http.server 8788
```
Open `http://127.0.0.1:8788/` in Chrome (WebSerial does not exist in Safari). This is a local mirror of solderless.engineering scraped 2026-05-18, so it works without network.

- [ ] **Step 3: Enter bootloader mode and confirm the device appears**

Power the puck off. Hold Track 1 + Track 4. Plug in USB-C. Release once the Track 1 LED lights. In the launcher, open **device info** and connect. Expected: the page reports device state rather than a connection error. Note what it prints in `docs/flashing.md`.

- [ ] **Step 4: Flash the looper build from Task 0.2**

Open **firmware utility**, select `refs/sp1-tape-looper/sp1_looper.bin`, connect, flash. (Task 0.2 proved a local build reproduces this file byte for byte, so there is nothing to gain from building it again first.) Expected: progress runs to 100 percent and the puck reboots into the looper (LED behaviour per the looper README).

- [ ] **Step 5: Recover, twice**

Re-enter bootloader mode from the running looper firmware (power off, Track 1 + Track 4, plug in). Flash the shipped `refs/sp1-tape-looper/sp1_looper.bin` this time. Then do it once more with your own build. Two successful re-entries with a custom image already resident is the actual thing being tested: that a running app cannot lock you out.

**STOP RULE:** if bootloader mode cannot be re-entered from a running custom firmware, stop the project here and report. Do not write firmware for a device you cannot recover. (Cleared on pop, 2026-08-08.)

**Before diagnosing a failure here, check the USB identity rather than the page.** `ioreg -p IOUSB -l -w 0 | grep kUSBProductString | grep -i "stem\|SP-1"` reports `stem player` for the bootloader and `SP-1 Audio` for the looper. The serial port changes with the mode and the flasher keeps holding the old one, so a successful recovery reads as `device mode: unknown (...)` until the page is reloaded and reconnected. That is not a failed recovery, and it cost us a scare.

- [ ] **Step 6: Write `docs/flashing.md`**

The literal steps that worked, the exact button hold, what the LEDs did at each stage, what the flasher printed, and how long it took. Add a "if it will not enter bootloader" section with whatever you learned.

- [ ] **Step 7: Commit**

```bash
git add docs/flashing.md
git commit -m "docs: flash and recovery drill executed on the dev puck"
```

---

### Task 1.2: Sync jack electrical survey and adapter

The spec's revised pre-flight #1 asks for four things specifically: pin order, idle polarity, source resistance and drive current, because the Tiliqua input is a current loop and not a logic-level UART line. This task answers all four.

**Deferrable.** Nothing before Task 3.1 depends on it, and with the USB MIDI sink in place the whole firmware can be built and tested without it. Do it when a multimeter is to hand. If you want a go/no-go answer sooner without one, Task 1.3's straight-cable attempt into a USB MIDI interface is a binary version of the same test: bytes or no bytes, with no numbers to interpret.

**Files:**
- Create: `docs/hardware-notes.md`

**Interfaces:**
- Produces: a confirmed contact map for the SP-1 sync jack and a physical TRS adapter, both documented. Phase 3 depends on the adapter existing.

- [ ] **Step 1: Confirm which jack is which**

The SP-1 has two 3.5 mm jacks. With the looper running and nothing playing, identify the sync jack (the non-audio one). Note how you told them apart.

- [ ] **Step 2: Measure pin order and idle polarity**

With a multimeter, referenced to USB-C shell ground: measure sleeve (expect continuity to ground, near 0 ohm), ring (expect a steady DC level, hypothesis 3.3 V, this is MIDI idle/mark), and tip (expect near 0 V while stopped, this is the PO sync line). Record the actual numbers.

Interpretation: a steady high on the ring at idle confirms the PNP stage described at `main.c:4405-4410` and that `MIDI_INVERT 1` is correct. A steady low at idle means the polarity is inverted, and Phase 3 will need `MIDI_INVERT 0`.

- [ ] **Step 3: Measure source resistance and available drive current**

This is the step that decides whether the adapter is safe and whether it will work at all.

Measure the open-circuit ring voltage (from Step 2), then load the ring to ground through a known resistor of about 220 ohm (matching what the Tiliqua presents) and measure the voltage again. Source resistance is `R_load * (V_open - V_loaded) / V_loaded`, and the loop current is `V_loaded / R_load`.

Interpret against the receiver: the Tiliqua's `H11L1SR2M` needs roughly 5 mA through its LED to switch reliably.
- 5 mA or more with a source resistance of a few tens of ohms: connect directly, no extra parts.
- Noticeably under 5 mA: the SP-1's own series resistance is doing too much. Do not simply connect and hope; note it and expect marginal or missing bytes in Task 1.3, then consider a buffer.
- Much more than 20 mA: unlikely given the 220R at the receiver, but if the open-circuit voltage turns out to be a battery rail rather than 3.3 V, recompute before connecting.

Record all three numbers. This is the measurement that protects the Tiliqua.

REVIEW, and this changes the recommendation: the MIDI electrical specification requires current limiting on the **transmitter** side, not only at the receiver. The Tiliqua's D1 protection diode plus its 220R means a wrong polarity yields silence rather than a damaged opto, so the receiver is not the part at risk. The part at risk is the scarce SP-1: if its ring is driven from a rail with little series resistance, a ring-to-sleeve short (which happens routinely while a TRS plug slides through its intermediate contacts on insertion) puts that short straight across the output stage.

So unless Step 3 measures a source resistance that already provides limiting, **put a resistor in the adapter on the SP-1 ring**. Size it from the measurement: enough total series resistance that a dead short is bounded to a safe current, while still delivering at least 5 mA through the receiver's LED. At 3.3 V with the receiver's 220R and roughly 1.2 V of LED drop, about 100R extra still yields around 6.5 mA. Prefer a working link with a resistor over a marginally brighter one without.

- [ ] **Step 4: Watch the ring while the looper transmits**

Start the looper's transport so it emits clock. Confirm on a **scope** that the ring is being modulated. REVIEW: a multimeter is not dependable here. MIDI clock is sparse (24 bytes per beat, each 320 us), so the shift in DC average is tiny and easily lost in meter averaging. If no scope is available, skip to Task 1.3 and let the byte-level monitor be the test: absence of movement on a meter proves nothing. If the ring never moves, the MIDI TX is either compiled out or on a different pin, and Task 1.3 will fail; note it now.

- [ ] **Step 5: Build the adapter**

Working hypothesis, to be tested in Task 1.3: **SP-1 ring to Tiliqua ring, SP-1 sleeve to Tiliqua tip, SP-1 tip unconnected.** Rationale: the Tiliqua input is an opto with 220R in series across the tip and ring of a TRS-A jack, so it needs a current loop. The SP-1 supplies the source on its ring and ground on its sleeve. Leaving the SP-1 tip disconnected also keeps the PO sync pulses out of the loop.

Build it from two 3.5 mm TRS pigtails joined with the mapping above (or a breakout board plus jumpers), using the current figure from Step 3 to decide whether any series resistance is needed.

- [ ] **Step 6: Ask the Discord if the schematic exists**

The sync jack is the only part of this hardware with no published documentation: it is absent from `SP-1-dev/src/stemplayer_pins.h` and its wiki page is a Todo. The looper's constants came from somewhere, and the TE SP-1 DEV Discord (linked from the SP-1-dev README) is the likely home of TimK's sync-jack schematic. One question there could replace an afternoon of probing. Ask, then continue regardless: measurement does not depend on an answer.

- [ ] **Step 7: Write `docs/hardware-notes.md`**

The jack identification, every measured voltage and current, the derived source resistance, the adapter wiring diagram in ASCII, and the reasoning above. Note explicitly which facts are measured and which are inherited from the looper's comments, since this is the one area where the community documentation runs out.

If the schematic does turn up, add it here and say so: it would also be the single most useful thing this project could contribute back to SP-1-dev.

- [ ] **Step 8: Commit**

```bash
git add docs/hardware-notes.md
git commit -m "docs: SP-1 sync jack survey and TRS adapter wiring"
```

---

### Task 1.3: v0a, prove real MIDI bytes leave the puck

**Files:**
- Modify: `docs/hardware-notes.md`

- [ ] **Step 1: Connect to a known-good MIDI input**

SP-1 sync jack, through the Task 1.2 adapter, into a USB MIDI interface that reaches the Mac.

**Use the cheap interface, never the Tiliqua, for this first connection.** Its input stage is the same standard opto arrangement, so it proves the same thing, and if something about the SP-1's drive turns out to be wrong it is a replaceable box rather than the r5 motherboard that finds out.

**Worth one free attempt before building anything:** connect the interface to the sync jack as directly as its form allows, a straight TRS cable if it has a socket, or plugged straight in if its TRS end is a plug. It costs nothing and the risk is low (a standard MIDI input is opto-isolated with a series resistor and a protection diode). If clock bytes appear, the wiring question is answered and Task 1.2's adapter is unnecessary.

**The interface is Type A** (established 2026-08-08): it works with Push 3, and Push 3's 3.5 mm MIDI ports are TRS Type A, MMA-compliant, with Ableton stating explicitly that Type B adapters do not work. The Tiliqua is Type A as well, which is why Push 3 drives it today. **One adapter therefore serves both**: build it against the cheap interface, then use it unchanged on the Tiliqua.

What that means concretely:

| | Type A input (interface and Tiliqua) | SP-1 sync jack |
|---|---|---|
| Tip | data / current return | Pocket Operator sync |
| Ring | +V current source | MIDI data |
| Sleeve | shield | ground (the return) |

Ring passes straight through; **tip and sleeve swap at one end**; the SP-1's tip is left unconnected so its PO pulses stay out of the loop. No standard part does this (Type A/B adapters swap tip and ring, not tip and sleeve), so it is two solderless screw-terminal 3.5 mm plugs and two wires, or two bare-wire pigtails twisted together.

Try the direct connection first anyway: some interfaces tie sleeve to circuit ground rather than leaving it floating as a shield, and if this one does, it may work with no adapter at all.

Expect it probably will not work, though, and the reason is worth knowing. A TRS-A input runs its opto between **tip and ring**, with the sleeve as shield. The SP-1 drives MIDI on its **ring** and returns to ground on its **sleeve** (its tip carries Pocket Operator sync). A straight cable therefore connects the SP-1's ring to the right place but leaves the loop open, because the return never reaches the interface's tip. Closing it needs tip and sleeve swapped at one end, which is not a standard adapter: it is two TRS pigtails joined, or a screw-terminal breakout at each end. No soldering required if the pigtails are bare-wire.

- [ ] **Step 2: Watch raw bytes**

```bash
brew install receivemidi
receivemidi list
receivemidi dev "<your interface name>" clock start stop
```
Alternatively use MIDI Monitor.app. Start the looper's transport.

Expected: a steady stream of clock messages at 24 per quarter note, plus start on transport start and stop on stop.

- [ ] **Step 3: If nothing arrives, work the decision tree in order**

1. Ring not modulating at all (from Task 1.2 Step 4): the TX is not running. Check `MIDI_SYNC_ENABLE` is 1 in the build (`main.c:4436`) and that the looper transport is actually running, since clock only flows when the engine or a tapped grid is active.
2. Bytes arrive but are garbage: polarity. Rebuild the looper with `MIDI_INVERT 0` (`main.c:4422`), reflash, retest. This is why Phase 0 came first.
3. Bytes arrive but the rate is wrong: baud. `MIDI_BIT_US` is 32 for 31250 (`main.c:4423`).
4. Nothing at all and the ring does modulate: the adapter mapping is wrong. Try SP-1 ring to Tiliqua tip with sleeve to ring. If that also fails, and Task 1.2 Step 3 measured under 5 mA, the loop is current-starved rather than miswired, which is a finding worth stopping on.

- [ ] **Step 4: Record the result**

Append to `docs/hardware-notes.md`: which wiring worked, which `MIDI_INVERT` value was needed, a copy of the monitor output. If `MIDI_INVERT 0` was needed, that value carries into Phase 3.

**STOP RULE:** if no combination produces clean MIDI bytes on a known-good receiver, stop and report. There is no point writing a controller for a link that cannot carry bytes.

- [ ] **Step 5: Commit**

```bash
git add docs/hardware-notes.md
git commit -m "docs: v0a proven, SP-1 emits valid MIDI over the sync jack"
```

---

### Task 1.4 (optional): v0b, zero-code end-to-end into the Tiliqua

Skip this if a polysyn r5 bitstream is not already available. Phase 3 proves the same path with a CC and no bitstream juggling.

- [ ] **Step 1: Flash polysyn to a spare Tiliqua slot**

Per the popgoblin project's conventions: build with `pdm polysyn build` if needed, flash the resulting archive to a slot with the dbg USB-C port connected.

- [ ] **Step 2: Connect and observe**

SP-1 sync jack through the adapter into the Tiliqua MIDI-in. Start the looper transport. Polysyn routes real-time clock through a divider and emits it as a square wave on **audio output channel 1** (`gateware/src/top/polysyn/top.py:637-639`). Watch that output on a scope or on the vectorscope.

Expected: a pulse train whose rate tracks the looper's tempo.

- [ ] **Step 3: Record in `docs/hardware-notes.md` and commit**

```bash
git add docs/hardware-notes.md
git commit -m "docs: v0b, MIDI clock confirmed arriving at the Tiliqua"
```

---

# Phase 2: Firmware skeleton

First custom code. The deliverable is a firmware that boots, shows life, charges, powers down cleanly, and can be recovered. No MIDI yet: nothing is worth debugging on top of an uncertain bring-up.

### Task 2.1: Buildable skeleton with watchdog and clean power-off

**Files:**
- Create: `boards/teenageengineering/stem_player/` (from `sp1-midi`, with two deltas)
- Create: `firmware/CMakeLists.txt`
- Create: `firmware/prj.conf`
- Create: `firmware/app.overlay`
- Create: `firmware/src/main.c`
- Create: `firmware/src/board_io.h`
- Create: `firmware/src/board_io.c`

**Interfaces:**
- Consumes: the toolchain from Task 0.2, the recovery procedure from Task 1.1.
- Produces:
  - `void board_io_init(void);`
  - `void board_io_feed_wdt(void);`
  - `bool board_io_function_held(void);`
  - `void board_io_power_off(void);` (never returns)
  - `int  board_io_read_fader(int idx);` (raw SAADC code, negative on error)
  - `int  board_io_read_track_ladder(void);` (raw code, negative on error)
  - `int  board_io_decode_track_button(int raw);` (-1 none, 0 to 3 tracks, 4 play)
  - `void board_io_led_set(int idx, uint8_t level);` (centre row, level 0 to 255)
  - `void board_io_track_led_set(int idx, bool on);`

- [ ] **Step 1: Take the board definition from sp1-midi, then apply the looper's two deltas**

```bash
mkdir -p boards/teenageengineering
cp -R refs/sp1-midi/boards/teenageengineering/stem_player boards/teenageengineering/
ls boards/teenageengineering/stem_player
```

Then edit, and put a comment at the top of `stem_player.dts` naming the source repo, the licence, and these two deliberate differences:

1. **Remove the PWM LED nodes.** The looper's board file documents why (`refs/sp1-tape-looper/boards/.../stem_player.dts:15-17`): the LEDs are driven as raw GPIO by a soft-PWM renderer, and a PWM peripheral owning those pins fights it. We transplant that renderer, so we inherit the constraint.
2. **Disable `uart0`.** P1.01 to P1.04 belong to the CYBT Bluetooth module. This firmware never touches it, and driving TX risks contention.

Keep `cdc_acm_uart0` and `chosen zephyr,console`: the serial console is the debug lifeline. REVIEW: with `CONFIG_USB_DEVICE_STACK_NEXT=y` and `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n`, the console does **not** come up by itself. The application must bring USB up, exactly as the looper does (`main.c:4864`, using the helper pulled in by `include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)` at `firmware/CMakeLists.txt:4`). Add that `include(...)` to our CMakeLists and call `sample_usbd_init_device()` then `usbd_enable()` from `board_io_init`, or Task 2.2's console will never enumerate and the whole characterisation step is unobservable. Remove any USB audio node if `sp1-midi`'s board declares one; this firmware has no audio path.

Cross-check the fader, LED, ladder and charger pins against `refs/SP-1-dev/src/stemplayer_pins.h:19-84`. All of them are published there and should match exactly. Any mismatch is a real finding: stop and resolve it before flashing.

**Decision gate:** if the board comes up cleanly through Step 9, continue on this base. If bring-up is flaky in ways that trace to the board definition rather than your own code, fall back to the spec's option B (fork the looper, delete the engine) and record why in `docs/hardware-notes.md`.

- [ ] **Step 2: Write `firmware/app.overlay`**

Copy the ADC channel block from `refs/sp1-tape-looper/firmware/app.overlay` verbatim (channels 2 to 6, the four faders plus battery) and the `zephyr_user` `io-channels` list, adjusting for whatever `sp1-midi`'s board file already declares so that channels are not defined twice. Do not change gain, reference or acquisition time: the button decode thresholds in Step 6 are calibrated to exactly this configuration.

- [ ] **Step 3: Write `firmware/prj.conf`**

```conf
# Bootloader-safe baseline. The SP-1 has no reset pin: the watchdog and a
# clean SYSTEM_OFF path are the only ways back to the bootloader.
CONFIG_WATCHDOG=y
CONFIG_REBOOT=y
CONFIG_POWEROFF=y
CONFIG_HW_STACK_PROTECTION=y
CONFIG_STACK_SENTINEL=y
CONFIG_ASSERT=n
CONFIG_LOG=n

CONFIG_CLOCK_CONTROL=y
CONFIG_CLOCK_CONTROL_NRF_K32SRC_SYNTH=y

# ADC for the button ladders and the four faders.
CONFIG_ADC=y
CONFIG_GPIO=y

# USB CDC console for diagnostics. Not required at runtime.
CONFIG_PRINTK=y
CONFIG_SERIAL=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_UART_LINE_CTRL=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_USB_DEVICE_STACK_NEXT=y
CONFIG_USBD_CDC_ACM_CLASS=y
CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n
CONFIG_HWINFO=y

# Flash writes for the preset page (Phase 7). FLASH_PAGE_LAYOUT is needed by
# flash_area_erase, and on nRF52840 the ARM MPU blocks runtime flash writes
# unless MPU_ALLOW_FLASH_WRITE is set: without it Phase 7 faults at the first
# save instead of failing cleanly.
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_FLASH_PAGE_LAYOUT=y
CONFIG_MPU_ALLOW_FLASH_WRITE=y

# The LED soft-PWM ISR must not be delayed by kernel locks, or low duty
# cycles visibly flicker. Same reasoning as the looper's dim-LED build.
CONFIG_ZERO_LATENCY_IRQS=y

CONFIG_MAIN_STACK_SIZE=2048
```

- [ ] **Step 4: Write `firmware/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1-remote)

# Pulls in sample_usbd_init_device()/usbd_enable(), which the CDC console
# needs because CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n means nothing
# brings USB up on its own. The looper does exactly this at its
# CMakeLists.txt:4. Omit it and the diagnostic console never enumerates.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

target_sources(app PRIVATE
  src/main.c
  src/board_io.c
  src/txqueue.c
  src/controls.c
  src/buttons.c
  src/profile.c
  src/presets.c
  src/presets_flash.c
  src/midi_tx.c
  src/midi_trs.c
  src/midi_usb.c
)
```

Later phases fill in most of these. To keep this task's firmware build green without breaking the "Expected: FAIL, X.h not found" checks that later tasks rely on, create **`.c` stubs only, with no headers**:

```c
/* Placeholder: filled in by Task N.N. */
typedef int sp1_placeholder_t;
```

A stub header would make every later host-test failure check wrong; a missing `.c` would break this build. This is the combination that satisfies both.

- [ ] **Step 5: Write `firmware/src/board_io.h`**

```c
/* Hardware access for the SP-1 panel. Everything Zephyr-specific lives
 * behind this header so the logic modules stay host-testable. */
#ifndef SP1_BOARD_IO_H
#define SP1_BOARD_IO_H

#include <stdbool.h>
#include <stdint.h>

#define BOARD_NUM_FADERS 4
#define BOARD_NUM_LEDS   4

void board_io_init(void);
void board_io_feed_wdt(void);

/* Raw 12-bit SAADC codes; negative means the read failed and the caller
 * should hold its last value. */
int  board_io_read_fader(int idx);
int  board_io_read_track_ladder(void);
int  board_io_decode_track_button(int raw);

bool board_io_function_held(void);
void board_io_power_off(void);

void board_io_led_set(int idx, uint8_t level);
void board_io_track_led_set(int idx, bool on);

#endif /* SP1_BOARD_IO_H */
```

- [ ] **Step 6: Write `firmware/src/board_io.c` by transplanting from the looper**

Transplant these blocks from `refs/sp1-tape-looper/firmware/src/main.c`, keeping an attribution comment with the line range on each. Prefer `sp1-midi`'s equivalents where it has one that is already a clean driver (its charger and watchdog subsystems in particular); take the looper's version where the constants are the value.

| What | Source lines | Notes |
|---|---|---|
| `struct led`, `leds[]`, `track_leds[]` | 100-110 | pin maps, cross-checked against SP-1-dev |
| power/function button pins | 121-123 | P0.27, active low with pull-up |
| BQ24232 charger pins | 126-129 | needed by power-off and by charging |
| `BTN_COM` rail | 145-146 | P1.10 must be high before sampling |
| `adc_ladder[]` and the `LAD_*` indices | 143-156 | keep the index meanings identical |
| `ladder_read()` | 192-208 | 2x oversample, returns -1 on error |
| `controls_init()` | 211 onward | raise `BTN_COM`, set up ADC channels |
| `decode_tracks()` | 5098-5107 | the calibrated thresholds, below |
| LED soft-PWM (`led_pwm_init`, ISR, `led_on`, `led_off`, `track_led_on`, `track_led_off`, all-off) | 5121-5320 | keep the zero-latency IRQ |
| `feed_wdt()` | 5565-5583 | feeds the timeout installed below |
| watchdog install (`wdt_install_timeout` + `wdt_setup`) | 5901-5906 | 4 s window. REVIEW: this is NOT inside the `feed_wdt` block; it sits in the boot sequence. Lifting only the 5524-5583 range would give you a fed watchdog that was never installed. |
| wake-on-button arming | 5585 onward | required by `SYSTEM_OFF` |
| `power_off()` | 5664-5735 | clears both LED rows, powers down the external chips, then clears `RESETREAS` (5730) and writes `SYSTEMOFF` (5732). REVIEW: the range must extend past 5720 or you get a power-off that never actually powers off. |
| **`enter_dfu()`** | 5743-5752 | REVIEW: the escape hatch, previously missed. Flushes, lights all four track LEDs as a cue, writes `GPREGRET = 0x57`, resets. |
| **the `enter_dfu` trigger** | 6981 | Track 1 + Track 4 held 1.2 s in the control loop. Reproduce this in `main.c`: it is the only recovery path that works when the app is running but wedged. |
| `g_resetreas` capture at boot | 85 and its boot-time read | BIG FIVE requirement |

The button decode, which is the one piece worth reproducing here because a wrong threshold is a silent bug:

```c
/* Transplanted from sp1-tape-looper firmware/src/main.c:5098-5107 (MIT).
 * Thresholds are calibrated to the exact ADC configuration in the board
 * files and app.overlay: gain 1/6, 0.6 V internal reference, 20 us
 * acquisition, 12-bit. Do not change one without re-measuring the other. */
int board_io_decode_track_button(int v)
{
    if (v <  110) return -1;   /* none           */
    if (v <  300) return 0;    /* track 1, ~213  */
    if (v <  560) return 1;    /* track 2, ~403  */
    if (v <  950) return 2;    /* track 3, ~733  */
    if (v < 1500) return 3;    /* track 4, ~1220 */
    return 4;                  /* play,    ~1823 */
}
```

Drop everything to do with audio, eMMC, I2S, I2C codecs and the looper engine. `board_io_led_set` takes a 0 to 255 level: map it onto whatever the transplanted soft-PWM exposes, and if the renderer turns out to be on/off only, threshold at 128 for now and note it as a Phase 6 item.

One thing that is easy to drop by accident: **charging**. The BQ24232's charge-enable pin (`BQ_NCE_PIN`, P0.21) is active low, so `board_io_init` must drive it low or the puck will never charge over USB-C, which the spec lists as expected behaviour. Verify before leaving this task, with an actual measurement rather than an impression: read the battery ADC (`LAD_BATT`, AIN4) over the console, note the raw code, leave USB-C connected for 15 minutes with the firmware running, and confirm the code has risen. Alternatively read the BQ24232's `nCHG` pin (P0.22, open-drain, LOW while charging) and confirm it is low. "It seems to charge" is not a check.

- [ ] **Step 7: Write `firmware/src/main.c`**

```c
/* SP-1 Remote: a 4-fader / 4-button TRS MIDI controller.
 *
 * BOOTLOADER SAFETY (the SP-1 "BIG FIVE", inherited from sp1-tape-looper):
 *   the app lives at 0x20000; the watchdog is fed every control pass;
 *   bootloader-owned clocks and peripherals are not re-initialised;
 *   SYSTEM_OFF is the only power-down path; RESETREAS is cleared at boot
 *   and again before SYSTEM_OFF. There is no reset pin on this hardware.
 */
#include <zephyr/kernel.h>
#include "board_io.h"

#define CONTROL_PERIOD_MS 5
#define POWER_HOLD_MS     2500

int main(void)
{
    board_io_init();

    uint32_t held_ms = 0;

    for (;;) {
        board_io_feed_wdt();

        if (board_io_function_held()) {
            held_ms += CONTROL_PERIOD_MS;
            if (held_ms >= POWER_HOLD_MS) {
                board_io_power_off();   /* does not return */
            }
        } else {
            held_ms = 0;
        }

        /* Proof of life until Phase 3: a slow heartbeat on LED 0. */
        board_io_led_set(0, ((k_uptime_get() / 500) & 1) ? 255 : 0);

        k_msleep(CONTROL_PERIOD_MS);
    }
    return 0;
}
```

- [ ] **Step 8: Build**

```bash
source ~/zephyrproject/.venv/bin/activate && export ZEPHYR_BASE=~/zephyrproject/zephyr
west build -p -b stem_player firmware -- -DBOARD_ROOT=$(pwd)
ls -l build/zephyr/zephyr.bin
```
Expected: a clean build producing a `zephyr.bin` well under `0xDF000`.

- [ ] **Step 9: Flash and verify on the dev puck**

Bootloader mode, flash `build/zephyr/zephyr.bin` with the firmware utility. Expected: LED 0 blinks at 1 Hz; holding the function button for 2.5 s powers the device off with all LEDs dark; the puck powers back on normally; USB-C charges it; bootloader mode still works.

**STOP RULE:** if the puck boots but cannot re-enter bootloader mode, this is the BIG FIVE failing. Recover via the drill from Task 1.1 and do not proceed until it works.

- [ ] **Step 10: Commit**

```bash
git add boards firmware
git commit -m "feat: bootable skeleton with watchdog, LEDs, charging and clean power-off"
```

---

### Task 2.2: Characterise the faders and buttons on real hardware

The spec's revised risk note is explicit: the faders are absolute-position sensors (Codex verified), and the real unknowns are resolution, jitter, touch and release behaviour, dead zones and calibration. Size the deadband from data measured here, not from the default.

**Files:**
- Modify: `firmware/src/main.c`
- Modify: `docs/hardware-notes.md`

- [ ] **Step 1: Add a diagnostic loop to `main.c`**

Behind `SP1_DIAG` so the release build in Phase 8 compiles it out:

```c
#define SP1_DIAG 1

#if SP1_DIAG
        static int diag_div;
        if (++diag_div >= 20) {            /* every 100 ms */
            diag_div = 0;
            int lad = board_io_read_track_ladder();
            printk("f0=%4d f1=%4d f2=%4d f3=%4d  lad=%4d btn=%d\n",
                   board_io_read_fader(0), board_io_read_fader(1),
                   board_io_read_fader(2), board_io_read_fader(3),
                   lad, board_io_decode_track_button(lad));
        }
#endif
```

- [ ] **Step 2: Build, flash, and read the console**

```bash
west build -p -b stem_player firmware -- -DBOARD_ROOT=$(pwd)
# flash, then:
ls /dev/tty.usbmodem*
screen /dev/tty.usbmodem<id> 115200
```

- [ ] **Step 3: Measure, do not eyeball**

Capture the console to a file and work out, per fader:
- **Range:** minimum and maximum raw code at the physical extremes. Expected near 0 and near 3700.
- **Jitter at rest:** leave everything untouched for 30 seconds and record the peak-to-peak spread. The looper reports plus or minus 1 count (`main.c:2685`); this is the number that sizes the deadband.
- **Dead zones:** sweep slowly end to end and look for raw values that never appear, or plateaus where the reading stops tracking the finger.
- **Touch and release:** does the reading hold when the finger lifts, or does it jump to a rest value? A jump on release would make these gesture sensors rather than positions, and Phase 4 would need rethinking.

- [ ] **Step 4: Size the deadband and record**

Set `FADER_DEADBAND_RAW` (Task 4.1) to about 4x the measured peak-to-peak jitter, with 8 as the default if jitter is the expected plus or minus 1. Keep it well under one 7-bit step, which is about 29 raw counts. Write every measured number into `docs/hardware-notes.md`, along with the chosen deadband and the reasoning.

If a fader's full-scale differs from 3700 by more than about 5 percent, change `FADER_RAW_FULL` to the measured value and say so.

- [ ] **Step 5: Verify the buttons**

Press each track button and play. Expected: the decoded index matches the physical button and releasing returns -1. Note any button whose raw code sits near a threshold boundary.

- [ ] **Step 6: Commit**

```bash
git add firmware/src docs/hardware-notes.md
git commit -m "feat: fader and button reads, characterised on hardware"
```

---

### Task 2.3: LED survey

**Files:**
- Modify: `docs/hardware-notes.md`
- Modify: `firmware/src/main.c` (temporary diagnostic, reverted at the end)

- [ ] **Step 1: Walk every LED**

Temporarily replace the control loop body with a loop that lights each of the eight known LEDs alone for 700 ms, in order: centre row 0 to 3, then track row 0 to 3.

- [ ] **Step 2: Watch the panel and write down what lights where**

Record, for each index, its physical position. Confirm whether any LED sits next to or along a fader, and confirm there is no addressable trail. If a trail exists and is not driven by any of the eight, note it as a finding: Phase 6 would then need a pin hunt, which is out of scope for v1.

- [ ] **Step 3: Check brightness control**

Set levels 32, 128 and 255 on centre LED 0 for two seconds each. Expected: visibly different brightness. If the renderer is on/off only, note that Phase 6 must render value as a blink rate or a bar across the four centre LEDs instead.

- [ ] **Step 4: Revert the diagnostic, keep the notes, commit**

```bash
git add docs/hardware-notes.md firmware/src/main.c
git commit -m "docs: LED map and brightness capability surveyed on hardware"
```

---

# Phase 3: Transmit path

The end-to-end proof, plus the queue that keeps the wire honest.

### Task 3.1: Bit-banged TRS MIDI transmitter

**Naming, because there are two sinks by the end of this phase:** `midi_trs_*` is the bit-banged TRS jack (this task), `midi_usb_*` is USB MIDI (Task 3.3), and `midi_tx_*` is the layer above both: the coalescing queue and the drain thread that fans out to them (Task 3.2). Callers outside this phase only ever use `midi_tx_send`.

**Files:**
- Create: `firmware/src/midi_trs.h`
- Create: `firmware/src/midi_trs.c`
- Modify: `firmware/src/main.c`

**Interfaces:**
- Produces:
  - `void midi_trs_init(void);`
  - `void midi_trs_send_byte(uint8_t b);`
  - `void midi_trs_send_cc(uint8_t channel, uint8_t cc, uint8_t value);` (channel is the wire value, 0 to 15)

- [ ] **Step 1: Write `firmware/src/midi_trs.h`**

```c
/* TRS MIDI transmit on the SP-1 sync jack. One of two sinks; the queue in
 * midi_tx.c fans out to this and to USB MIDI.
 *
 * The sync jack's ring is driven by P0.23 (BC807 base) through a PNP that
 * INVERTS the line, so the waveform is bit-banged rather than handed to a
 * UART peripheral. A hardware timer clocks one bit per ISR with interrupts
 * left on. Transplanted from sp1-tape-looper firmware/src/main.c:4398-4530
 * (MIT). */
#ifndef SP1_MIDI_TRS_H
#define SP1_MIDI_TRS_H

#include <stdint.h>

void midi_trs_init(void);
void midi_trs_send_byte(uint8_t b);
void midi_trs_send_cc(uint8_t channel, uint8_t cc, uint8_t value);

#endif /* SP1_MIDI_TRS_H */
```

- [ ] **Step 2: Write `firmware/src/midi_trs.c`**

Transplant `midi_pins_init`, `midi_line`, `midi_timer_isr`, `midi_timer_init` and `midi_send` from `main.c:4419-4530` unchanged, keeping the constants:

```c
#define MIDI_PIN     23u   /* P0.23 BC807 base, drives SYNC_RING */
#define MIDI_INVERT  1     /* the PNP stage inverts; set from docs/hardware-notes.md */
#define MIDI_BIT_US  32u   /* 31250 baud */
#define MIDI_TIMER      NRF_TIMER2
#define MIDI_TIMER_IRQn TIMER2_IRQn
```

Set `MIDI_INVERT` to whatever Task 1.3 recorded as working. Drop the Pocket-Operator sync pins entirely: this firmware does not emit PO sync, and leaving `POSYNC_PIN` undriven keeps the tip out of the current loop.

Then add the only new code in this file:

```c
void midi_trs_send_cc(uint8_t channel, uint8_t cc, uint8_t value)
{
    midi_trs_send_byte((uint8_t)(0xB0u | (channel & 0x0Fu)));
    midi_trs_send_byte(cc & 0x7Fu);
    midi_trs_send_byte(value & 0x7Fu);
}
```

`midi_trs_init` is not free either: REVIEW flagged that the plan called it without defining it. It must bring up the pins and the timer. The queue and its drain thread are initialised separately, in `midi_tx_init` (Task 3.2):

```c
void midi_trs_init(void)
{
    midi_pins_init();     /* transplanted, main.c:4443-4460 */
    midi_timer_init();    /* transplanted, main.c:4515-4527 */
}
```

`midi_trs_send_byte` is the transplanted `midi_send`, renamed. Note in a comment that each byte occupies 320 us on the wire (10 bits at 32 us), so a three-byte CC is just under 1 ms. That number is the basis of the queue sizing in Task 3.2.

- [ ] **Step 3: Add a MIDI smoke test to `main.c`**

Behind `SP1_DIAG`, send `midi_trs_send_cc(0, 102, v)` once every 100 ms with `v` ramping 0 to 127 and back, so cutoff sweeps continuously without touching anything.

- [ ] **Step 4: Build and flash**

```bash
west build -p -b stem_player firmware -- -DBOARD_ROOT=$(pwd)
```

- [ ] **Step 5: Verify on the MIDI monitor first**

Same rig as Task 1.3. Expected: a stream of `channel 1 control-change 102` messages with a ramping value, roughly 10 per second, no framing errors. Debug here, not at the rack: this is a two-cable bench setup.

- [ ] **Step 6: Verify on the Tiliqua**

SP-1 into the Tiliqua MIDI-in with popgoblin running. Expected: the cutoff option value sweeps on the display, and the MIDI activity indicator shows traffic. REVIEW: that indicator drives the motherboard LEDs via the PCA9635 (`gateware/src/rs/lib/src/ui.rs:115`), not an on-screen glyph, so watch the hardware LEDs rather than hunting for something on the video output. The moving cutoff value is the unambiguous signal either way. This is the end-to-end proof the spec's v0 was after.

- [ ] **Step 7: Record and commit**

Append the result to `docs/hardware-notes.md`, including the final `MIDI_INVERT` value.

```bash
git add firmware/src/midi_trs.c firmware/src/midi_trs.h firmware/src/main.c docs/hardware-notes.md
git commit -m "feat: bit-banged TRS MIDI TX, cutoff sweep confirmed on the synth"
```

---

### Task 3.2: The coalescing transmit queue

The spec is specific about this: one queue, latest value per CC wins, never a blocking polled send from the control loop. Four faders at one message per 10 ms each would be 400 messages per second against a wire that carries about 1000, and a preset burst on top of a sweep would queue behind it. Coalescing makes the queue depth bounded by the number of distinct CCs in flight rather than by how fast a finger moves.

**Files:**
- Create: `firmware/src/txqueue.h`
- Create: `firmware/src/txqueue.c`
- Create: `tests/host/test_txqueue.c`
- Modify: `tests/host/Makefile`
- Modify: `firmware/src/midi_tx.c` (add the drain thread)

**Interfaces:**
- Produces:
  - `void txq_init(txqueue_t *q);`
  - `bool txq_push(txqueue_t *q, cc_msg_t m);`
  - `bool txq_pop(txqueue_t *q, cc_msg_t *out);`
  - `uint8_t txq_count(const txqueue_t *q);`
  - and in `midi_tx.h`: `void midi_tx_send(cc_msg_t m);`, the only function the rest of the firmware calls to transmit.

**Build ordering matters here.** `cc_msg_t` lives in its own tiny header, `firmware/src/cc_msg.h`, created as Step 0 of this task. It is deliberately NOT in `profile.h`: the queue is built in Phase 3 and the profile table in Phase 4, so putting the shared type in the later file would make this task unbuildable. Everything downstream picks it up transitively.

- [ ] **Step 0: Write `firmware/src/cc_msg.h`**

```c
/* PURE. One MIDI control-change message.
 *
 * Its own header, and not part of profile.h, purely for build ordering:
 * the transmit queue (Phase 3) needs this type but must not depend on the
 * profile table (Phase 4). Everything downstream gets it transitively. */
#ifndef SP1_CC_MSG_H
#define SP1_CC_MSG_H

#include <stdint.h>

typedef struct {
    uint8_t channel;
    uint8_t cc;
    uint8_t value;
} cc_msg_t;

#endif /* SP1_CC_MSG_H */
```

- [ ] **Step 1: Write the failing test, `tests/host/test_txqueue.c`**

```c
#include "txqueue.h"
#include "test_util.h"

static txqueue_t q;

static void test_fifo_order(void)
{
    txq_init(&q);
    CHECK(txq_push(&q, (cc_msg_t){ 0, 102, 10 }));
    CHECK(txq_push(&q, (cc_msg_t){ 0, 104, 20 }));
    CHECK_EQ(txq_count(&q), 2);

    cc_msg_t m;
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.cc, 102);
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.cc, 104);
    CHECK(!txq_pop(&q, &m));
}

/* The whole point: a fader swept faster than the wire drains must not
 * queue every intermediate value. The newest value replaces the pending
 * one and the queue depth stays at 1. */
static void test_same_cc_coalesces_to_the_latest_value(void)
{
    txq_init(&q);
    for (uint8_t v = 0; v < 100; v++) {
        CHECK(txq_push(&q, (cc_msg_t){ 0, 102, v }));
    }
    CHECK_EQ(txq_count(&q), 1);

    cc_msg_t m;
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.value, 99);
}

/* Coalescing must not reorder: a busy fader cannot push a waiting button
 * message to the back of the queue forever. */
static void test_coalescing_keeps_queue_position(void)
{
    txq_init(&q);
    txq_push(&q, (cc_msg_t){ 0, 102, 1 });
    txq_push(&q, (cc_msg_t){ 0,  64, 127 });
    txq_push(&q, (cc_msg_t){ 0, 102, 9 });
    CHECK_EQ(txq_count(&q), 2);

    cc_msg_t m;
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.cc, 102);
    CHECK_EQ(m.value, 9);
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.cc, 64);
}

static void test_same_cc_on_a_different_channel_is_a_different_message(void)
{
    txq_init(&q);
    txq_push(&q, (cc_msg_t){ 0, 102, 1 });
    txq_push(&q, (cc_msg_t){ 1, 102, 2 });
    CHECK_EQ(txq_count(&q), 2);
}

static void test_full_queue_rejects_new_but_still_coalesces(void)
{
    txq_init(&q);
    for (uint8_t i = 0; i < TXQ_MAX; i++) {
        CHECK(txq_push(&q, (cc_msg_t){ 0, (uint8_t)(1 + i), i }));
    }
    CHECK_EQ(txq_count(&q), TXQ_MAX);

    /* A brand new CC has nowhere to go. */
    CHECK(!txq_push(&q, (cc_msg_t){ 0, 99, 5 }));
    /* One already queued still updates in place. */
    CHECK(txq_push(&q, (cc_msg_t){ 0, 1, 77 }));
    CHECK_EQ(txq_count(&q), TXQ_MAX);

    cc_msg_t m;
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.cc, 1);
    CHECK_EQ(m.value, 77);
}

static void test_wraps_around_the_ring(void)
{
    txq_init(&q);
    cc_msg_t m;
    for (int cycle = 0; cycle < 5; cycle++) {
        for (uint8_t i = 0; i < TXQ_MAX; i++) {
            CHECK(txq_push(&q, (cc_msg_t){ 0, (uint8_t)(1 + i), i }));
        }
        for (uint8_t i = 0; i < TXQ_MAX; i++) {
            CHECK(txq_pop(&q, &m));
            CHECK_EQ(m.cc, 1 + i);
        }
        CHECK_EQ(txq_count(&q), 0);
    }
}

int main(void)
{
    RUN(test_fifo_order);
    RUN(test_same_cc_coalesces_to_the_latest_value);
    RUN(test_coalescing_keeps_queue_position);
    RUN(test_same_cc_on_a_different_channel_is_a_different_message);
    RUN(test_full_queue_rejects_new_but_still_coalesces);
    RUN(test_wraps_around_the_ring);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
sed -i '' 's/^TESTS := smoke/TESTS := smoke txqueue/' tests/host/Makefile
make -C tests/host test
```
Expected: FAIL, `txqueue.h` not found.

- [ ] **Step 3: Write `firmware/src/txqueue.h`**

```c
/* PURE. The single coalescing transmit queue.
 *
 * Every message the firmware sends goes through here. Pushing a (channel,
 * cc) that is already pending overwrites its value IN PLACE and keeps its
 * position, so a fast fader sweep collapses to one message per drain
 * without ever pushing a waiting button message to the back.
 *
 * NOT thread-safe by itself: the control loop pushes and the transmit
 * thread pops, so both must hold the same mutex (see midi_tx.c). Keeping
 * the locking out here is what allows host testing. */
#ifndef SP1_TXQUEUE_H
#define SP1_TXQUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include "cc_msg.h"

#define TXQ_MAX 16

typedef struct {
    cc_msg_t item[TXQ_MAX];
    uint8_t  head;
    uint8_t  count;
} txqueue_t;

void    txq_init(txqueue_t *q);
bool    txq_push(txqueue_t *q, cc_msg_t m);
bool    txq_pop(txqueue_t *q, cc_msg_t *out);
uint8_t txq_count(const txqueue_t *q);

#endif /* SP1_TXQUEUE_H */
```

- [ ] **Step 4: Write `firmware/src/txqueue.c`**

```c
#include "txqueue.h"

void txq_init(txqueue_t *q)
{
    q->head  = 0;
    q->count = 0;
}

bool txq_push(txqueue_t *q, cc_msg_t m)
{
    for (uint8_t i = 0; i < q->count; i++) {
        uint8_t idx = (uint8_t)((q->head + i) % TXQ_MAX);
        if (q->item[idx].cc == m.cc && q->item[idx].channel == m.channel) {
            q->item[idx].value = m.value;
            return true;
        }
    }

    if (q->count >= TXQ_MAX) {
        return false;
    }

    uint8_t tail = (uint8_t)((q->head + q->count) % TXQ_MAX);
    q->item[tail] = m;
    q->count++;
    return true;
}

bool txq_pop(txqueue_t *q, cc_msg_t *out)
{
    if (q->count == 0) {
        return false;
    }
    *out    = q->item[q->head];
    q->head = (uint8_t)((q->head + 1) % TXQ_MAX);
    q->count--;
    return true;
}

uint8_t txq_count(const txqueue_t *q)
{
    return q->count;
}
```

- [ ] **Step 5: Run the tests**

```bash
make -C tests/host test
```
Expected: PASS for `smoke` and `txqueue`.

- [ ] **Step 6: Add the drain thread to `midi_tx.c`**

```c
/* The queue and its drain thread. The control loop only ever calls
 * midi_tx_send, which takes the mutex, pushes, and returns immediately.
 * The thread does the blocking part: one CC is about 1 ms on the wire.
 *
 * Priority: below the LED soft-PWM ISR (which is zero-latency and
 * unaffected) and above nothing in particular. A cooperative-range
 * priority would let a long burst starve the control loop, so use a
 * preemptible priority and let the scheduler interleave. */
#define MIDI_TX_STACK 512
#define MIDI_TX_PRIO  7
/* One message per 5 ms = 200/s, matching popgoblin's drain rate exactly. */
#define MIDI_TX_SPACING_MS 5

static txqueue_t     tx_q;
static struct k_mutex tx_lock;
static struct k_sem   tx_wake;
static K_THREAD_STACK_DEFINE(tx_stack, MIDI_TX_STACK);
static struct k_thread tx_tcb;

void midi_tx_send(cc_msg_t m)
{
    k_mutex_lock(&tx_lock, K_FOREVER);
    bool ok = txq_push(&tx_q, m);
    k_mutex_unlock(&tx_lock);
    if (ok) {
        k_sem_give(&tx_wake);
    }
    /* A rejected push means 16 distinct CCs are already pending, which
     * the surface cannot produce. Dropping is correct: the next value
     * for that CC will coalesce anyway. */
}

static void midi_tx_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (;;) {
        cc_msg_t m;
        k_mutex_lock(&tx_lock, K_FOREVER);
        bool have = txq_pop(&tx_q, &m);
        k_mutex_unlock(&tx_lock);

        if (!have) {
            k_sem_take(&tx_wake, K_FOREVER);
            continue;
        }

        midi_trs_send_cc(m.channel, m.cc, m.value);
        /* Pace to the RECEIVER, not to the wire. popgoblin's MIDI FIFO is
         * 8 entries (top.py:150) and its firmware pops exactly one per
         * 5 ms timer ISR (main.rs:28,100), so anything faster than 200
         * messages per second can silently overflow it. The wire would
         * carry ~1000. Coalescing plus this pacing is what keeps a
         * four-fader sweep inside what the synth can actually absorb. */
        k_msleep(MIDI_TX_SPACING_MS);
    }
}
```

`midi_tx_init` owns the queue, the mutex, the semaphore and the thread, and calls `midi_trs_init()` (and later `midi_usb_init()`) itself, so `main.c` only ever calls `midi_tx_init()`:

```c
void midi_tx_init(void)
{
    midi_trs_init();
    txq_init(&tx_q);
    k_mutex_init(&tx_lock);
    k_sem_init(&tx_wake, 0, 1);
    k_thread_create(&tx_tcb, tx_stack, MIDI_TX_STACK, midi_tx_thread,
                    NULL, NULL, NULL, MIDI_TX_PRIO, 0, K_NO_WAIT);
}
```

Create `firmware/src/midi_tx.h` with `midi_tx_init`, `midi_tx_send`, and an include of `txqueue.h`.

- [ ] **Step 7: Verify on hardware that nothing regressed**

Change the Task 3.1 smoke test to call `midi_tx_send((cc_msg_t){0, 102, v})` instead of `midi_trs_send_cc` directly. Expected: identical behaviour on the monitor and on the synth. Then push values in faster than the queue drains (call `midi_tx_send` every 1 ms) and confirm two things: the monitor shows roughly **200 messages per second, not 1000**, because the drain thread paces to the synth's 5 ms ISR, and the values arriving **step in jumps of about 6 rather than replaying every intermediate value**. That jumpiness IS the coalescing working. What you must not see is a backlog that keeps arriving after you stop feeding it.

- [ ] **Step 8: Commit**

```bash
git add firmware/src/txqueue.c firmware/src/txqueue.h firmware/src/midi_tx.c \
        firmware/src/midi_tx.h tests/host
git commit -m "feat: coalescing transmit queue with a drain thread"
```

---

### Task 3.3: USB MIDI as a second sink

Everything from here on can then be developed and watched at the desk with one USB-C cable, instead of needing the hand-built adapter or the rack. That matters because the TRS jack is the one part of this hardware with no published documentation, and without this task every behaviour bug in Phases 4 to 7 is entangled with it.

**Files:**
- Create: `firmware/src/midi_usb.h`
- Create: `firmware/src/midi_usb.c`
- Modify: `firmware/src/midi_tx.c` (fan out to both sinks)
- Modify: `firmware/prj.conf`, `firmware/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `void midi_usb_init(void);`
  - `void midi_usb_send_cc(uint8_t channel, uint8_t cc, uint8_t value);`
  - `bool midi_usb_ready(void);`

- [ ] **Step 1: Enable the class**

```conf
# USB MIDI 2 alongside the CDC console: a composite device. sp1-midi ships
# exactly this pair and the looper independently proves composite works on
# this hardware (it runs UAC2 + CDC).
CONFIG_USBD_MIDI2_CLASS=y
CONFIG_MIDI2_UMP_STREAM_RESPONDER=y
```

- [ ] **Step 2: Write `firmware/src/midi_usb.c`**

Lift the send path from `refs/sp1-midi/app/MidiController.cpp:12-18`, which is C++ but trivially transliterated. The whole thing is:

```c
#include <zephyr/usb/class/usbd_midi2.h>
#include <zephyr/audio/midi.h>
#include "midi_usb.h"

#define UMP_GROUP 0

static const struct device *midi_dev;
static atomic_t usb_ready;

void midi_usb_send_cc(uint8_t channel, uint8_t cc, uint8_t value)
{
    /* Silently drop when no host is listening. A puck on battery at the
     * rack is the normal case, not an error. */
    if (!atomic_get(&usb_ready) || midi_dev == NULL) {
        return;
    }
    const struct midi_ump ump = UMP_MIDI1_CHANNEL_VOICE(
        UMP_GROUP, UMP_MIDI_CONTROL_CHANGE, channel & 0x0Fu,
        cc & 0x7Fu, value & 0x7Fu);
    usbd_midi_send(midi_dev, ump);
}
```

`midi_usb_init` fetches the device and registers the ready callback that sets `usb_ready`. Follow `sp1-midi/app/main.cpp` for the registration order relative to `usbd_enable()`, since the CDC console shares the same USB context.

- [ ] **Step 3: Fan out in the drain thread**

One queue, two sinks. In `midi_tx_thread`, replace the single send with:

```c
        midi_trs_send_cc(m.channel, m.cc, m.value);
        midi_usb_send_cc(m.channel, m.cc, m.value);
        k_msleep(MIDI_TX_SPACING_MS);
```

Keep the 5 ms pacing even though USB does not need it. It exists for popgoblin's FIFO, and one pace for both sinks means what you watch on the Mac is exactly what the synth receives, which is the entire point of using USB as the development view.

- [ ] **Step 4: Verify both sinks at once**

Connect USB-C to the Mac AND the TRS adapter to a MIDI interface. Run the Task 3.1 ramp.

Expected: the same CC 102 ramp arrives on both, at the same rate, with the same values. The SP-1 should appear by name in the Mac's MIDI device list.

- [ ] **Step 5: The test that justifies caution — does USB traffic corrupt the TRS bits?**

This is the one real risk in adding USB, and it needs an explicit check. The TRS transmitter is bit-banged: a hardware timer drives one bit per ISR at 32 us spacing, with interrupts left on (`main.c:4425-4435`). USB adds a SOF interrupt every 1 ms plus transfer interrupts, any of which can delay a bit edge. Enough delay corrupts the byte's framing.

With USB connected and enumerated, and ideally with the host also polling, run the ramp for several minutes into the MIDI monitor and watch for framing errors, dropped bytes or wrong values on the **TRS** side.

Evidence it should be fine: the looper ran isochronous USB audio (UAC2, far heavier than MIDI) alongside this same timer bit-bang, and its changelog treats the MIDI clock as usable. But that is inference, and a corrupted byte here is exactly the sort of thing that gets blamed on the cable at the rack three weeks later. Measure it.

If it does corrupt: the fallback is to suppress USB sends while a TRS byte is in flight, or to accept USB as a bench-only mode compiled out of the release build. Record which, and why, in `docs/hardware-notes.md`.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/midi_usb.c firmware/src/midi_usb.h firmware/src/midi_tx.c \
        firmware/prj.conf firmware/CMakeLists.txt docs/hardware-notes.md
git commit -m "feat: USB MIDI as a second sink, so behaviour is testable at the desk"
```

**Contingency:** if Task 1.3 failed or was inconclusive, pull this task ahead of 3.1. USB MIDI needs nothing from the sync jack, so the entire firmware can be built and validated while the TRS electrical question stays open.

---

# Phase 4: Faders to CC

### Task 4.1: Fader conditioning and soft pickup, pure logic

**Files:**
- Create: `firmware/src/controls.h`
- Create: `firmware/src/controls.c`
- Create: `tests/host/test_controls.c`
- Modify: `tests/host/Makefile` (add `controls` to `TESTS`)

**Interfaces:**
- Produces:
  - `uint8_t fader_raw_to_cc(int raw);`
  - `bool fader_update(fader_state_t *st, int raw, uint8_t *out_value);`
  - `void fader_arm_pickup(fader_state_t *st, uint8_t target);`
  - `bool fader_pickup_armed(const fader_state_t *st);`
  - `uint8_t btn_update(btn_state_t *st, bool pressed_now, uint32_t now_ms);`

No duration is returned, because nothing consumes one: the only timed gesture left is hold-to-save, and `BTN_EV_HOLD` fires while the button is still down.

Note there is no timestamp parameter: rate limiting lives in the transmit queue now, not per fader.

- [ ] **Step 1: Write the failing test, `tests/host/test_controls.c`**

```c
#include "controls.h"
#include "test_util.h"

static void test_raw_to_cc_endpoints(void)
{
    CHECK_EQ(fader_raw_to_cc(0), 0);
    CHECK_EQ(fader_raw_to_cc(FADER_RAW_FULL), 127);
    CHECK_EQ(fader_raw_to_cc(FADER_RAW_FULL + 500), 127);
    CHECK_EQ(fader_raw_to_cc(-1), 0);
}

static void test_raw_to_cc_midpoint(void)
{
    CHECK_EQ(fader_raw_to_cc(1850), 64);
}

/* The puck must be silent at power-on: it has no idea what the synth is
 * set to, and blasting four CCs from wherever the faders happen to sit
 * would stamp on the current patch. The first reading only seeds. */
static void test_first_reading_seeds_silently(void)
{
    fader_state_t st = {0};
    uint8_t v = 0xFF;
    CHECK(!fader_update(&st, 2000, &v));
    CHECK_EQ(v, 0xFF);
}

static void test_first_real_move_emits(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 2000, &v));
    CHECK(fader_update(&st, 2100, &v));
    CHECK_EQ(v, fader_raw_to_cc(2100));
}

static void test_adc_error_never_emits(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, -1, &v));
    CHECK(!st.have_seed);
}

static void test_jitter_inside_deadband_is_ignored(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 1000, &v));
    CHECK(!fader_update(&st, 1004, &v));
    CHECK(!fader_update(&st, 996,  &v));
}

/* A 7-bit step spans about 29 raw counts and CC 34 covers raw 976 to 1005,
 * so a move of 20 counts inside that window clears the 8-count deadband and
 * still lands on the same CC. That is the only way to reach the duplicate
 * suppression branch: a smaller move is rejected by the deadband first and
 * never gets there. */
static void test_same_cc_value_is_not_resent(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 970, &v));     /* seed */
    CHECK(fader_update(&st, 980, &v));      /* emits CC 34 */
    CHECK_EQ(v, 34);
    CHECK_EQ(fader_raw_to_cc(1000), 34);    /* same bucket... */
    CHECK(!fader_update(&st, 1000, &v));    /* ...so nothing is resent */
}

/* --- soft pickup after a preset recall --- */

static void test_pickup_suppresses_until_the_fader_crosses(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 300, &v));          /* seed low */
    CHECK(fader_update(&st, 400, &v));           /* now sending, cc ~14 */

    fader_arm_pickup(&st, 100);                  /* recall put the synth at 100 */
    CHECK(fader_pickup_armed(&st));

    /* Moving up but still well below 100: nothing goes out. */
    CHECK(!fader_update(&st, 1000, &v));
    CHECK(!fader_update(&st, 2000, &v));
    CHECK(fader_pickup_armed(&st));

    /* Crossing the recalled value catches it and takes over. */
    CHECK(fader_update(&st, 3000, &v));
    CHECK(!fader_pickup_armed(&st));
    CHECK_EQ(v, fader_raw_to_cc(3000));
}

static void test_pickup_not_armed_when_already_at_the_target(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 1000, &v));
    CHECK(fader_update(&st, 1100, &v));
    fader_arm_pickup(&st, st.last_sent);
    CHECK(!fader_pickup_armed(&st));
}

static void test_pickup_from_above_also_catches(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 3600, &v));
    CHECK(fader_update(&st, 3500, &v));
    fader_arm_pickup(&st, 40);
    CHECK(fader_pickup_armed(&st));
    CHECK(!fader_update(&st, 2000, &v));         /* cc ~69, still above 40 */
    CHECK(fader_update(&st, 900, &v));           /* cc ~31, crossed below */
    CHECK(!fader_pickup_armed(&st));
}

/* Stopping exactly ON the recalled value is "caught" too. Arming adopts the
 * target as last_sent, so without resolving pickup before the duplicate
 * check this fader would stay armed forever, blinking its LED while already
 * in agreement with the synth. */
static void test_pickup_landing_exactly_on_target_disarms(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 300, &v));
    CHECK(fader_update(&st, 400, &v));

    uint8_t target = fader_raw_to_cc(2000);
    fader_arm_pickup(&st, target);
    CHECK(fader_pickup_armed(&st));

    /* Land precisely on it: nothing to send, but pickup must let go. */
    CHECK(!fader_update(&st, 2000, &v));
    CHECK(!fader_pickup_armed(&st));

    /* And the fader is live again immediately afterwards. */
    CHECK(fader_update(&st, 2400, &v));
}

static void test_arming_pickup_adopts_the_recalled_value(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 1000, &v));
    CHECK(fader_update(&st, 1100, &v));
    fader_arm_pickup(&st, 77);
    CHECK_EQ(st.last_sent, 77);
}

/* --- buttons --- */

static void test_press_then_short_release_is_a_tap(void)
{
    btn_state_t st = {0};
    CHECK_EQ(btn_update(&st, true, 0), BTN_EV_PRESS);
    CHECK_EQ(btn_update(&st, true, 50), 0);
    CHECK_EQ(btn_update(&st, false, 120), BTN_EV_RELEASE | BTN_EV_TAP);
}

static void test_hold_fires_once_and_release_is_not_a_tap(void)
{
    btn_state_t st = {0};
    CHECK_EQ(btn_update(&st, true, 0), BTN_EV_PRESS);
    CHECK_EQ(btn_update(&st, true, BTN_HOLD_MS - 1), 0);
    CHECK_EQ(btn_update(&st, true, BTN_HOLD_MS), BTN_EV_HOLD);
    CHECK_EQ(btn_update(&st, true, BTN_HOLD_MS + 500), 0);
    CHECK_EQ(btn_update(&st, false, BTN_HOLD_MS + 600), BTN_EV_RELEASE);
}

/* The hold fires while the button is still DOWN, which is why the save
 * gesture does not depend on a release arriving either. */
static void test_hold_fires_while_still_held(void)
{
    btn_state_t st = {0};
    CHECK_EQ(btn_update(&st, true, 1000), BTN_EV_PRESS);
    CHECK_EQ(btn_update(&st, true, 1000 + BTN_HOLD_MS - 1), 0);
    CHECK_EQ(btn_update(&st, true, 1000 + BTN_HOLD_MS), BTN_EV_HOLD);
}

/* The control loop's uptime counter is 32-bit milliseconds, which wraps
 * after about 49 days. Unsigned subtraction makes the wrap a non-event,
 * but only if nobody compares timestamps directly. */
static void test_hold_detection_survives_the_millisecond_wrap(void)
{
    btn_state_t st = {0};
    uint32_t near_wrap = 0xFFFFFF00u;
    CHECK_EQ(btn_update(&st, true, near_wrap), BTN_EV_PRESS);
    CHECK_EQ(btn_update(&st, true, near_wrap + 100u), 0);
    CHECK_EQ(btn_update(&st, true, near_wrap + BTN_HOLD_MS), BTN_EV_HOLD);
}

static void test_idle_button_reports_nothing(void)
{
    btn_state_t st = {0};
    CHECK_EQ(btn_update(&st, false, 0), 0);
    CHECK_EQ(btn_update(&st, false, 5000), 0);
}

int main(void)
{
    RUN(test_raw_to_cc_endpoints);
    RUN(test_raw_to_cc_midpoint);
    RUN(test_first_reading_seeds_silently);
    RUN(test_first_real_move_emits);
    RUN(test_adc_error_never_emits);
    RUN(test_jitter_inside_deadband_is_ignored);
    RUN(test_same_cc_value_is_not_resent);
    RUN(test_pickup_suppresses_until_the_fader_crosses);
    RUN(test_pickup_not_armed_when_already_at_the_target);
    RUN(test_pickup_from_above_also_catches);
    RUN(test_pickup_landing_exactly_on_target_disarms);
    RUN(test_arming_pickup_adopts_the_recalled_value);
    RUN(test_press_then_short_release_is_a_tap);
    RUN(test_hold_fires_once_and_release_is_not_a_tap);
    RUN(test_hold_fires_while_still_held);
    RUN(test_hold_detection_survives_the_millisecond_wrap);
    RUN(test_idle_button_reports_nothing);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
sed -i '' 's/^TESTS := smoke txqueue/TESTS := smoke txqueue controls/' tests/host/Makefile
make -C tests/host test
```
Expected: FAIL, `controls.h` not found.

- [ ] **Step 3: Write `firmware/src/controls.h`**

```c
/* PURE LOGIC. No Zephyr headers may ever appear in this file: it is
 * compiled into the host unit tests as well as into the firmware. */
#ifndef SP1_CONTROLS_H
#define SP1_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

#define FADER_RAW_FULL          3700
#define FADER_DEADBAND_RAW      8

typedef struct {
    int      last_raw;
    uint8_t  last_sent;
    bool     have_seed;
    bool     have_sent;
    bool     pickup_armed;
    uint8_t  pickup_target;
    int8_t   pickup_side;
} fader_state_t;

uint8_t fader_raw_to_cc(int raw);
bool fader_update(fader_state_t *st, int raw, uint8_t *out_value);
void fader_arm_pickup(fader_state_t *st, uint8_t target);
bool fader_pickup_armed(const fader_state_t *st);

/* Hold-to-save a preset. The ONLY duration this firmware measures, and it
 * fires while the button is still down, so nothing depends on a release
 * arriving. */
#define BTN_HOLD_MS        2000

#define BTN_EV_PRESS    0x01u
#define BTN_EV_HOLD     0x02u
#define BTN_EV_TAP      0x04u
#define BTN_EV_RELEASE  0x08u

typedef struct {
    bool     down;
    bool     hold_fired;
    uint32_t down_at_ms;
} btn_state_t;

/* Feed the debounced pressed/released state once per control pass. */
uint8_t btn_update(btn_state_t *st, bool pressed_now, uint32_t now_ms);

#endif /* SP1_CONTROLS_H */
```

- [ ] **Step 4: Write `firmware/src/controls.c`**

```c
#include "controls.h"

uint8_t fader_raw_to_cc(int raw)
{
    if (raw <= 0) {
        return 0;
    }
    if (raw >= FADER_RAW_FULL) {
        return 127;
    }
    return (uint8_t)(((raw * 127) + (FADER_RAW_FULL / 2)) / FADER_RAW_FULL);
}

static int abs_diff(int a, int b)
{
    int d = a - b;
    return d < 0 ? -d : d;
}

bool fader_update(fader_state_t *st, int raw, uint8_t *out_value)
{
    if (raw < 0) {
        return false;
    }

    if (!st->have_seed) {
        st->have_seed = true;
        st->last_raw  = raw;
        return false;
    }

    if (abs_diff(raw, st->last_raw) < FADER_DEADBAND_RAW) {
        return false;
    }
    st->last_raw = raw;

    uint8_t v = fader_raw_to_cc(raw);

    /* Pickup is resolved BEFORE the duplicate-value check. Arming sets
     * last_sent to the recalled target, so a fader that lands exactly ON
     * the target would otherwise be rejected as a duplicate and stay armed
     * forever, blinking its LED while it is in fact already in agreement. */
    if (st->pickup_armed) {
        int side = (v > st->pickup_target) - (v < st->pickup_target);
        if (side != 0 && side == st->pickup_side) {
            return false;           /* still on the far side: suppressed */
        }
        st->pickup_armed = false;   /* crossed or landed on it: caught */
    }

    if (st->have_sent && v == st->last_sent) {
        return false;
    }

    st->last_sent = v;
    st->have_sent = true;
    *out_value    = v;
    return true;
}

void fader_arm_pickup(fader_state_t *st, uint8_t target)
{
    uint8_t cur = st->have_sent ? st->last_sent
                                : fader_raw_to_cc(st->last_raw);

    st->last_sent = target;
    st->have_sent = true;

    if (!st->have_seed || cur == target) {
        st->pickup_armed = false;
        return;
    }

    st->pickup_target = target;
    st->pickup_side   = (cur > target) ? 1 : -1;
    st->pickup_armed  = true;
}

bool fader_pickup_armed(const fader_state_t *st)
{
    return st->pickup_armed;
}

uint8_t btn_update(btn_state_t *st, bool pressed_now, uint32_t now_ms)
{
    uint8_t ev = 0;

    if (pressed_now && !st->down) {
        st->down       = true;
        st->hold_fired = false;
        st->down_at_ms = now_ms;
        return BTN_EV_PRESS;
    }

    if (pressed_now && st->down) {
        /* Unsigned subtraction, so the 32-bit millisecond wrap is a
         * non-event. Fires once per press. */
        if (!st->hold_fired &&
            (uint32_t)(now_ms - st->down_at_ms) >= BTN_HOLD_MS) {
            st->hold_fired = true;
            ev |= BTN_EV_HOLD;
        }
        return ev;
    }

    if (!pressed_now && st->down) {
        st->down = false;
        ev = BTN_EV_RELEASE;
        if (!st->hold_fired) {
            ev |= BTN_EV_TAP;
        }
        return ev;
    }

    return 0;
}
```

- [ ] **Step 5: Run the tests**

```bash
make -C tests/host test
```
Expected: PASS for `smoke`, `txqueue` and `controls`.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/controls.c firmware/src/controls.h tests/host
git commit -m "feat: fader conditioning, silent boot and soft pickup"
```

---

### Task 4.2: The profile table

**Files:**
- Create: `firmware/src/profile.h`
- Create: `firmware/src/profile.c`
- Create: `tests/host/test_profile.c`
- Modify: `tests/host/Makefile`

**Interfaces:**
- Produces: `cc_msg_t`, `profile_t`, `fader_cfg_t`, `button_cfg_t`, `btn_mode_t`, and `extern const profile_t profile_popgoblin_default;`. Everything downstream reads this table and never hard-codes a CC number.

Mode names follow the spec's vocabulary: *toggle* (freeze), *cycle* (shimmer), *preset*.

- [ ] **Step 1: Write the failing test, `tests/host/test_profile.c`**

```c
#include "profile.h"
#include "test_util.h"
#include <stdbool.h>

static void test_default_profile_matches_the_spec(void)
{
    const profile_t *p = &profile_popgoblin_default;

    CHECK_EQ(p->fader[0].cc, 102);
    CHECK_EQ(p->fader[1].cc, 104);
    CHECK_EQ(p->fader[2].cc, 107);
    CHECK_EQ(p->fader[3].cc, 108);

    for (int i = 0; i < PROFILE_NUM_FADERS; i++) {
        CHECK_EQ(p->fader[i].channel, 0);
    }

    CHECK_EQ(p->button[0].mode, BTN_MODE_TOGGLE);
    CHECK_EQ(p->button[0].cc,   64);
    CHECK_EQ(p->button[1].mode, BTN_MODE_CYCLE);
    CHECK_EQ(p->button[1].cc,   105);
    CHECK_EQ(p->button[1].n_steps, 4);
    CHECK_EQ(p->button[2].mode, BTN_MODE_PRESET);
    CHECK_EQ(p->button[2].preset_slot, 0);
    CHECK_EQ(p->button[3].mode, BTN_MODE_PRESET);
    CHECK_EQ(p->button[3].preset_slot, 1);
}

/* PopGoblin's ShimmerLevel iterates Boost, Full, Low, Off and set_from_cc
 * picks index = cc * 4 / 128. So the buckets are Boost 0-31, Full 32-63,
 * Low 64-95, Off 96-127, and the spec's off -> low -> full -> boost order
 * means DESCENDING CC values at the bucket centres. Getting this backwards
 * would silently invert the control. */
static void test_shimmer_steps_hit_the_right_enum_buckets(void)
{
    const button_cfg_t *b = &profile_popgoblin_default.button[1];
    CHECK_EQ(b->n_steps, 4);

    const char *name[4] = { "off", "low", "full", "boost" };
    const int   want[4] = { 3, 2, 1, 0 };   /* enum index the synth will pick */
    for (int i = 0; i < 4; i++) {
        int bucket = (b->steps[i] * 4) / 128;
        (void)name;
        CHECK_EQ(bucket, want[i]);
    }

    /* And the puck must start where the synth's default is: Full. */
    CHECK_EQ(b->init_step, 2);
    CHECK_EQ((b->steps[b->init_step] * 4) / 128, 1);   /* 1 == Full */
}

static void test_preset_capture_list_uses_existing_ccs_only(void)
{
    const profile_t *p = &profile_popgoblin_default;
    CHECK_EQ(p->preset_capture_len, 5);
    for (int i = 0; i < p->preset_capture_len; i++) {
        uint8_t cc = p->preset_capture[i];
        CHECK(cc != 64);
        bool known = false;
        for (int f = 0; f < PROFILE_NUM_FADERS; f++) {
            if (p->fader[f].cc == cc) known = true;
        }
        if (p->button[1].cc == cc) known = true;
        CHECK(known);
    }
}

int main(void)
{
    RUN(test_default_profile_matches_the_spec);
    RUN(test_shimmer_steps_hit_the_right_enum_buckets);
    RUN(test_preset_capture_list_uses_existing_ccs_only);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
sed -i '' 's/^TESTS := smoke txqueue controls/TESTS := smoke txqueue controls profile/' tests/host/Makefile
make -C tests/host test
```
Expected: FAIL, `profile.h` not found.

- [ ] **Step 3: Write `firmware/src/profile.h`**

```c
/* PURE. The whole control surface in one table.
 *
 * This firmware is a generic 4-fader / 4-button MIDI controller. The table
 * below is the shipped default profile, which happens to target the
 * PopGoblin string synth. Changing the surface means changing this table
 * and nothing else. A browser-based editor over WebSerial that rewrites
 * this table at runtime is parked for v1.1.
 *
 * BTN_MODE_LIST and the per-button list[] exist because the frozen spec
 * requires "per-button CC list + channel" in the config table. */
#ifndef SP1_PROFILE_H
#define SP1_PROFILE_H

#include <stdint.h>
#include "cc_msg.h"

#define PROFILE_NUM_FADERS   4
#define PROFILE_NUM_BUTTONS  4
#define PROFILE_MAX_STEPS    4
#define PROFILE_MAX_CAPTURE  5
#define PROFILE_MAX_BTN_LIST 5

typedef enum {
    BTN_MODE_TOGGLE  = 0,
    BTN_MODE_CYCLE   = 1,
    BTN_MODE_PRESET  = 2,
    /* Emit a fixed list of (cc, value) pairs configured in the table. This
     * is what makes the surface generic: any button can be any list. */
    BTN_MODE_LIST    = 3,
} btn_mode_t;

typedef struct {
    uint8_t cc;
    uint8_t channel;
} fader_cfg_t;

typedef struct {
    btn_mode_t mode;
    uint8_t    channel;
    uint8_t    cc;
    uint8_t    on_value;
    uint8_t    off_value;
    uint8_t    steps[PROFILE_MAX_STEPS];
    uint8_t    n_steps;
    uint8_t    init_step;
    uint8_t    preset_slot;
    cc_msg_t   list[PROFILE_MAX_BTN_LIST];
    uint8_t    n_list;
} button_cfg_t;

typedef struct {
    fader_cfg_t  fader[PROFILE_NUM_FADERS];
    button_cfg_t button[PROFILE_NUM_BUTTONS];
    uint8_t      preset_capture[PROFILE_MAX_CAPTURE];
    uint8_t      preset_capture_len;
} profile_t;

extern const profile_t profile_popgoblin_default;

#endif /* SP1_PROFILE_H */
```

- [ ] **Step 4: Write `firmware/src/profile.c`**

```c
#include "profile.h"

const profile_t profile_popgoblin_default = {
    .fader = {
        { .cc = 102, .channel = 0 },
        { .cc = 104, .channel = 0 },
        { .cc = 107, .channel = 0 },
        { .cc = 108, .channel = 0 },
    },
    .button = {
        {
            .mode = BTN_MODE_TOGGLE, .channel = 0, .cc = 64,
            .on_value = 127, .off_value = 0,
        },
        {
            /* shimmer. PopGoblin's ShimmerLevel enum iterates
             * Boost, Full, Low, Off and set_from_cc picks
             * index = cc * 4 / 128, so the buckets are
             * Boost 0-31, Full 32-63, Low 64-95, Off 96-127.
             * Send bucket CENTRES, ordered off -> low -> full -> boost
             * per the spec. The synth's default is Full, which is why
             * init_step is 2: the puck starts in agreement without
             * transmitting anything. */
            .mode = BTN_MODE_CYCLE, .channel = 0, .cc = 105,
            .steps = { 112, 80, 48, 16 }, .n_steps = 4, .init_step = 2,
        },
        { .mode = BTN_MODE_PRESET, .channel = 0, .preset_slot = 0 },
        { .mode = BTN_MODE_PRESET, .channel = 0, .preset_slot = 1 },
    },
    .preset_capture     = { 102, 104, 107, 108, 105 },
    .preset_capture_len = 5,
};
```

- [ ] **Step 5: Run the tests**

```bash
make -C tests/host test
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/profile.c firmware/src/profile.h tests/host
git commit -m "feat: profile table with the PopGoblin default surface"
```

---

### Task 4.3: Wire the faders to the queue

**Files:**
- Modify: `firmware/src/main.c`

- [ ] **Step 1: Rewrite the control loop**

```c
#include <zephyr/kernel.h>
#include "board_io.h"
#include "controls.h"
#include "midi_tx.h"
#include "profile.h"

#define CONTROL_PERIOD_MS 5
#define POWER_HOLD_MS     2500

static fader_state_t g_fader[PROFILE_NUM_FADERS];

int main(void)
{
    const profile_t *prof = &profile_popgoblin_default;

    board_io_init();
    midi_tx_init();

    uint32_t held_ms = 0;

    for (;;) {
        board_io_feed_wdt();

        if (board_io_function_held()) {
            held_ms += CONTROL_PERIOD_MS;
            if (held_ms >= POWER_HOLD_MS) {
                board_io_power_off();
            }
        } else {
            held_ms = 0;
        }

        for (int i = 0; i < PROFILE_NUM_FADERS; i++) {
            uint8_t v;
            if (fader_update(&g_fader[i], board_io_read_fader(i), &v)) {
                midi_tx_send((cc_msg_t){ prof->fader[i].channel,
                                         prof->fader[i].cc, v });
            }
        }

        k_msleep(CONTROL_PERIOD_MS);
    }
    return 0;
}
```

`midi_tx_send` pushes and returns, so a fader sweep never blocks this loop and never delays the watchdog. All four faders are read every pass: the looper round-robins them only because its ADC reads compete with an eMMC streamer, which this firmware does not have. If the console shows the loop overrunning, fall back to round-robin.

- [ ] **Step 2: Build, flash, and test at the desk over USB**

From here on the fastest loop is USB-C to the Mac with a MIDI monitor open, no adapter and no rack. Validate on TRS at the end of the phase, not for every edit.

Expected, in order:
1. Power on and touch nothing: **no MIDI at all**. This is the silent-boot rule and it is easy to get wrong.
2. Move fader 1: CC 102 messages appear. Fader 2 gives 104, fader 3 gives 107, fader 4 gives 108.
3. Stop moving: messages stop. Any idle chatter means the deadband is too small for this unit, so raise `FADER_DEADBAND_RAW` and re-run the host tests.
4. Sweep all four at once, fast: the monitor should show a smooth interleaved stream rather than a backlog that keeps arriving after your hands stop.

- [ ] **Step 3: Test at the rack**

Expected: each fader moves its parameter on the popgoblin display, sweeps are smooth, and releasing a fader leaves the parameter exactly where the fader is.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/main.c
git commit -m "feat: four faders drive their profile CCs through the queue"
```

---

# Phase 5: Buttons

### Task 5.1: The unified button behaviour model, pure logic

`btn_update` already exists from Task 4.1. This task adds the layer above it.

**Files:**
- Create: `firmware/src/buttons.h`
- Create: `firmware/src/buttons.c`
- Create: `tests/host/test_buttons.c`
- Modify: `tests/host/Makefile`

**Interfaces:**
- Produces:
  - `void button_engine_init(button_engine_t *e, const profile_t *prof);`
  - `int  button_engine_event(button_engine_t *e, int idx, uint8_t ev, cc_msg_t *out, int out_max);`
  - `int  button_engine_pending_save_slot(const button_engine_t *e);`
  - `void button_engine_set_preset(button_engine_t *e, int slot, const cc_msg_t *msgs, int len);`
  - `const preset_slot_t *button_engine_get_preset(const button_engine_t *e, int slot);`
  - `void button_engine_sync_cycle(button_engine_t *e, int idx, uint8_t value);`
  - `uint8_t button_engine_step_index(const button_engine_t *e, int idx);`
  - `bool button_engine_is_latched(const button_engine_t *e, int idx);`

Everything a button does is "emit a list of (channel, cc, value)". The engine never talks to hardware and never sleeps: the caller queues the list.

- [ ] **Step 1: Write the failing test, `tests/host/test_buttons.c`**

```c
#include "buttons.h"
#include "profile.h"
#include "test_util.h"

#define OUT_MAX 8

static button_engine_t eng;
static cc_msg_t out[OUT_MAX];

static void setup(void)
{
    button_engine_init(&eng, &profile_popgoblin_default);
}

/* --- freeze: a plain toggle on the press --- */

static void test_freeze_press_turns_on(void)
{
    setup();
    int n = button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].cc, 64);
    CHECK_EQ(out[0].value, 127);
    CHECK(button_engine_is_latched(&eng, 0));
}

static void test_freeze_second_press_turns_off(void)
{
    setup();
    button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);
    int n = button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].value, 0);
    CHECK(!button_engine_is_latched(&eng, 0));
}

/* THE POINT OF THE TOGGLE DESIGN: release is inert, so a release that is
 * late, early, or never delivered at all cannot strand freeze in the on
 * state. The earlier timed-release design could, and that was the single
 * most session-ruining failure mode in the surface. */
static void test_freeze_ignores_release_entirely(void)
{
    setup();
    button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);

    CHECK_EQ(button_engine_event(&eng, 0, BTN_EV_RELEASE, out, OUT_MAX), 0);
    CHECK_EQ(button_engine_event(&eng, 0, BTN_EV_RELEASE | BTN_EV_TAP,
                                 out, OUT_MAX), 0);
    CHECK_EQ(button_engine_event(&eng, 0, BTN_EV_HOLD, out, OUT_MAX), 0);
    /* Still on, whatever the release did or did not do. */
    CHECK(button_engine_is_latched(&eng, 0));
}

/* A press whose release is lost still toggles correctly next time: the
 * state machine only ever advances on a press. */
static void test_freeze_survives_a_lost_release(void)
{
    setup();
    button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);   /* on */
    /* release lost entirely: no event delivered at all */
    int n = button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].value, 0);                                  /* off */
}

/* --- shimmer: each press steps --- */

static void test_shimmer_steps_and_wraps(void)
{
    setup();
    const uint8_t want[5] = { 16, 112, 80, 48, 16 };
    for (int i = 0; i < 5; i++) {
        int n = button_engine_event(&eng, 1, BTN_EV_PRESS, out, OUT_MAX);
        CHECK_EQ(n, 1);
        CHECK_EQ(out[0].cc, 105);
        CHECK_EQ(out[0].value, want[i]);
    }
}

static void test_shimmer_release_sends_nothing(void)
{
    setup();
    CHECK_EQ(button_engine_event(&eng, 1, BTN_EV_RELEASE | BTN_EV_TAP,
                                 out, OUT_MAX), 0);
}

/* PopGoblin boots with shimmer at Full. The puck must start on that same
 * step so its first press is a real change and its LED does not lie, and
 * it must reach that agreement WITHOUT transmitting. */
static void test_cycle_starts_on_the_synths_default_step(void)
{
    setup();
    CHECK_EQ(button_engine_step_index(&eng, 1), 2);
}

/* A recall replays CC 105, which moves shimmer on the synth. If the puck
 * does not re-sync, its LED lies and the next press jumps from a stale
 * position, and a later snapshot captures the stale step. */
static void test_recall_resyncs_the_cycle_step(void)
{
    setup();
    CHECK_EQ(button_engine_step_index(&eng, 1), 2);      /* Full at boot */

    button_engine_sync_cycle(&eng, 1, 112);              /* recall sent Off */
    CHECK_EQ(button_engine_step_index(&eng, 1), 0);

    /* Nearest-match, not exact-match: a value between buckets still lands
     * on the step the synth will have chosen. */
    button_engine_sync_cycle(&eng, 1, 50);               /* nearest 48 = Full */
    CHECK_EQ(button_engine_step_index(&eng, 1), 2);

    /* And the next press steps on from there, not from the stale index. */
    int n = button_engine_event(&eng, 1, BTN_EV_PRESS, out, OUT_MAX);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].value, 16);                          /* Full -> Boost */
}

static void test_sync_cycle_ignores_non_cycle_buttons(void)
{
    setup();
    button_engine_sync_cycle(&eng, 0, 99);   /* freeze is a toggle */
    button_engine_sync_cycle(&eng, 9, 99);   /* out of range */
    CHECK(!button_engine_is_latched(&eng, 0));
}

/* --- presets: the only mode that waits for a release --- */

static void test_preset_tap_replays_the_stored_list(void)
{
    setup();
    cc_msg_t scene[3] = {
        { 0, 102, 30 }, { 0, 104, 90 }, { 0, 107, 12 },
    };
    button_engine_set_preset(&eng, 0, scene, 3);
    int n = button_engine_event(&eng, 2, BTN_EV_RELEASE | BTN_EV_TAP,
                                out, OUT_MAX);
    CHECK_EQ(n, 3);
    CHECK_EQ(out[0].cc, 102);
    CHECK_EQ(out[0].value, 30);
    CHECK_EQ(out[2].cc, 107);
    CHECK_EQ(out[2].value, 12);
}

static void test_preset_press_alone_does_not_replay(void)
{
    setup();
    cc_msg_t scene[1] = { { 0, 102, 30 } };
    button_engine_set_preset(&eng, 0, scene, 1);
    CHECK_EQ(button_engine_event(&eng, 2, BTN_EV_PRESS, out, OUT_MAX), 0);
}

static void test_empty_preset_sends_nothing(void)
{
    setup();
    CHECK_EQ(button_engine_event(&eng, 2, BTN_EV_RELEASE | BTN_EV_TAP,
                                 out, OUT_MAX), 0);
}

static void test_preset_hold_requests_a_save(void)
{
    setup();
    int n = button_engine_event(&eng, 3, BTN_EV_HOLD, out, OUT_MAX);
    CHECK_EQ(n, BUTTON_ACTION_SAVE_PRESET);
    CHECK_EQ(button_engine_pending_save_slot(&eng), 1);
}

static void test_release_after_a_save_does_not_also_replay(void)
{
    setup();
    button_engine_event(&eng, 3, BTN_EV_HOLD, out, OUT_MAX);
    CHECK_EQ(button_engine_event(&eng, 3, BTN_EV_RELEASE, out, OUT_MAX), 0);
}

static void test_negative_preset_length_is_refused(void)
{
    setup();
    cc_msg_t scene[1] = { { 0, 102, 30 } };
    button_engine_set_preset(&eng, 0, scene, -1);
    const preset_slot_t *p = button_engine_get_preset(&eng, 0);
    CHECK_EQ(p->len, 0);
}

int main(void)
{
    RUN(test_freeze_press_turns_on);
    RUN(test_freeze_second_press_turns_off);
    RUN(test_freeze_ignores_release_entirely);
    RUN(test_freeze_survives_a_lost_release);
    RUN(test_shimmer_steps_and_wraps);
    RUN(test_shimmer_release_sends_nothing);
    RUN(test_cycle_starts_on_the_synths_default_step);
    RUN(test_recall_resyncs_the_cycle_step);
    RUN(test_sync_cycle_ignores_non_cycle_buttons);
    RUN(test_preset_tap_replays_the_stored_list);
    RUN(test_preset_press_alone_does_not_replay);
    RUN(test_empty_preset_sends_nothing);
    RUN(test_preset_hold_requests_a_save);
    RUN(test_release_after_a_save_does_not_also_replay);
    RUN(test_negative_preset_length_is_refused);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run and watch it fail**

```bash
sed -i '' 's/^TESTS := smoke txqueue controls profile/TESTS := smoke txqueue controls profile buttons/' tests/host/Makefile
make -C tests/host test
```
Expected: FAIL, `buttons.h` not found.

- [ ] **Step 3: Write `firmware/src/buttons.h`**

```c
/* PURE. The unified button model: every button is "send a list of
 * (channel, cc, value)". Freeze is a one-item toggling list, shimmer is a
 * one-item cycling list, a preset is a short list replayed on the CCs the
 * faders already drive, and BTN_MODE_LIST is an arbitrary configured list.
 * No new CCs, and therefore no synth-side work. */
#ifndef SP1_BUTTONS_H
#define SP1_BUTTONS_H

#include <stdbool.h>
#include <stdint.h>
#include "controls.h"
#include "profile.h"

#define BUTTON_MAX_PRESET_SLOTS 2

/* Returned by button_engine_event instead of a message count. */
#define BUTTON_ACTION_SAVE_PRESET (-1)

typedef struct {
    cc_msg_t msg[PROFILE_MAX_CAPTURE];
    uint8_t  len;
} preset_slot_t;

typedef struct {
    const profile_t *prof;
    bool     latched[PROFILE_NUM_BUTTONS];    /* TOGGLE state */
    uint8_t  step_idx[PROFILE_NUM_BUTTONS];   /* CYCLE position */
    bool     save_armed[PROFILE_NUM_BUTTONS]; /* a hold consumed the release */
    int      pending_save_slot;
    preset_slot_t preset[BUTTON_MAX_PRESET_SLOTS];
} button_engine_t;

void button_engine_init(button_engine_t *e, const profile_t *prof);

/* Feed one button event mask from btn_update. Returns the number of
 * messages written to out, 0 for nothing to send, or
 * BUTTON_ACTION_SAVE_PRESET when the caller must snapshot the surface into
 * button_engine_pending_save_slot(e) and then call button_engine_set_preset.
 *
 * No duration parameter, deliberately. Nothing here reacts to HOW LONG a
 * button was held: TOGGLE and CYCLE act on the press, PRESET acts on the
 * HOLD event (which fires while the button is still down) or on the tap.
 * That is what makes a lost RELEASE harmless. */
int  button_engine_event(button_engine_t *e, int idx, uint8_t ev,
                         cc_msg_t *out, int out_max);

int  button_engine_pending_save_slot(const button_engine_t *e);
void button_engine_set_preset(button_engine_t *e, int slot,
                              const cc_msg_t *msgs, int len);
const preset_slot_t *button_engine_get_preset(const button_engine_t *e, int slot);

/* Re-sync a cycle button after a preset recall replayed its CC: pick the
 * step whose value is nearest what was just sent. Without this the puck's
 * step index, its LED, and the next press all disagree with the synth, and
 * the next snapshot captures the stale step. */
void button_engine_sync_cycle(button_engine_t *e, int idx, uint8_t value);

/* For the LED layer. */
uint8_t button_engine_step_index(const button_engine_t *e, int idx);
bool    button_engine_is_latched(const button_engine_t *e, int idx);

#endif /* SP1_BUTTONS_H */
```

- [ ] **Step 4: Write `firmware/src/buttons.c`**

```c
#include <string.h>
#include "buttons.h"

void button_engine_init(button_engine_t *e, const profile_t *prof)
{
    memset(e, 0, sizeof(*e));
    e->prof = prof;

    for (int i = 0; i < PROFILE_NUM_BUTTONS; i++) {
        const button_cfg_t *cfg = &prof->button[i];
        /* Start a cycle button where the synth actually is, so the puck
         * agrees with it at power-on without transmitting. */
        if (cfg->mode == BTN_MODE_CYCLE && cfg->n_steps) {
            e->step_idx[i] = (uint8_t)(cfg->init_step % cfg->n_steps);
        }
        /* Factory-default preset content, overwritten later by whatever
         * flash holds. */
        if (cfg->mode == BTN_MODE_PRESET && cfg->n_list) {
            button_engine_set_preset(e, cfg->preset_slot, cfg->list,
                                     cfg->n_list);
        }
    }
    e->pending_save_slot = -1;
}

static int emit(cc_msg_t *out, int out_max, int n,
                uint8_t ch, uint8_t cc, uint8_t val)
{
    if (n >= out_max) {
        return n;
    }
    out[n].channel = ch;
    out[n].cc      = cc;
    out[n].value   = val;
    return n + 1;
}

int button_engine_event(button_engine_t *e, int idx, uint8_t ev,
                        cc_msg_t *out, int out_max)
{
    if (idx < 0 || idx >= PROFILE_NUM_BUTTONS) {
        return 0;
    }
    const button_cfg_t *cfg = &e->prof->button[idx];
    int n = 0;

    switch (cfg->mode) {

    case BTN_MODE_TOGGLE:
        /* Plain alternation on the press, and nothing at all on release.
         * The synth treats CC 64 as a level (>= 64 is on), so the puck
         * simply alternates between the two levels.
         *
         * Release is deliberately inert. An earlier design timed the
         * release to offer a momentary pedal as well, which meant a lost
         * RELEASE left freeze on for the rest of the session with no way
         * back. Here a lost PRESS costs one more press and nothing else,
         * and a dropped message re-syncs on the next press. */
        if (ev & BTN_EV_PRESS) {
            e->latched[idx] = !e->latched[idx];
            return emit(out, out_max, n, cfg->channel, cfg->cc,
                        e->latched[idx] ? cfg->on_value : cfg->off_value);
        }
        return 0;

    case BTN_MODE_CYCLE:
        /* Also on the press: a stepped parameter should move under the
         * finger, not when it leaves. */
        if (ev & BTN_EV_PRESS) {
            if (cfg->n_steps == 0) {
                return 0;
            }
            e->step_idx[idx] = (uint8_t)((e->step_idx[idx] + 1) % cfg->n_steps);
            return emit(out, out_max, n, cfg->channel, cfg->cc,
                        cfg->steps[e->step_idx[idx]]);
        }
        return 0;

    case BTN_MODE_LIST:
        if (ev & BTN_EV_PRESS) {
            for (int i = 0; i < cfg->n_list; i++) {
                n = emit(out, out_max, n, cfg->list[i].channel,
                         cfg->list[i].cc, cfg->list[i].value);
            }
            return n;
        }
        return 0;

    case BTN_MODE_PRESET:
        /* The one mode that MUST wait for the release, because a press
         * alone cannot yet be distinguished from the start of a
         * hold-to-save. The HOLD event fires while the button is still
         * down, so the save does not depend on a release either. */
        if (ev & BTN_EV_HOLD) {
            e->save_armed[idx]   = true;
            e->pending_save_slot = cfg->preset_slot;
            return BUTTON_ACTION_SAVE_PRESET;
        }
        if (ev & BTN_EV_RELEASE) {
            if (e->save_armed[idx]) {
                e->save_armed[idx] = false;  /* the hold already acted */
                return 0;
            }
            if (!(ev & BTN_EV_TAP)) {
                return 0;
            }
            int slot = cfg->preset_slot;
            if (slot < 0 || slot >= BUTTON_MAX_PRESET_SLOTS) {
                return 0;
            }
            const preset_slot_t *p = &e->preset[slot];
            for (int i = 0; i < p->len; i++) {
                n = emit(out, out_max, n, p->msg[i].channel,
                         p->msg[i].cc, p->msg[i].value);
            }
            return n;
        }
        return 0;
    }
    return 0;
}

int button_engine_pending_save_slot(const button_engine_t *e)
{
    return e->pending_save_slot;
}

void button_engine_set_preset(button_engine_t *e, int slot,
                              const cc_msg_t *msgs, int len)
{
    if (slot < 0 || slot >= BUTTON_MAX_PRESET_SLOTS) {
        return;
    }
    if (len < 0) {
        return;                 /* a negative length would make memcpy enormous */
    }
    if (len > PROFILE_MAX_CAPTURE) {
        len = PROFILE_MAX_CAPTURE;
    }
    memcpy(e->preset[slot].msg, msgs, (size_t)len * sizeof(cc_msg_t));
    e->preset[slot].len  = (uint8_t)len;
    e->pending_save_slot = -1;
}

const preset_slot_t *button_engine_get_preset(const button_engine_t *e, int slot)
{
    if (slot < 0 || slot >= BUTTON_MAX_PRESET_SLOTS) {
        return NULL;
    }
    return &e->preset[slot];
}

void button_engine_sync_cycle(button_engine_t *e, int idx, uint8_t value)
{
    if (idx < 0 || idx >= PROFILE_NUM_BUTTONS) {
        return;
    }
    const button_cfg_t *cfg = &e->prof->button[idx];
    if (cfg->mode != BTN_MODE_CYCLE || cfg->n_steps == 0) {
        return;
    }

    uint8_t best = 0;
    int best_d = 256;
    for (uint8_t i = 0; i < cfg->n_steps; i++) {
        int d = (int)cfg->steps[i] - (int)value;
        if (d < 0) {
            d = -d;
        }
        if (d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    e->step_idx[idx] = best;
}

uint8_t button_engine_step_index(const button_engine_t *e, int idx)
{
    return (idx >= 0 && idx < PROFILE_NUM_BUTTONS) ? e->step_idx[idx] : 0u;
}

bool button_engine_is_latched(const button_engine_t *e, int idx)
{
    return (idx >= 0 && idx < PROFILE_NUM_BUTTONS) ? e->latched[idx] : false;
}
```

- [ ] **Step 5: Run the tests**

```bash
make -C tests/host test
```
Expected: PASS for all five suites.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/buttons.c firmware/src/buttons.h tests/host
git commit -m "feat: unified button model, freeze toggle, cycle and preset"
```

---

### Task 5.2: Wire the buttons, with debounce

**Files:**
- Modify: `firmware/src/main.c`

- [ ] **Step 1: Check the recovery combo BEFORE decoding any button**

This is the single most dangerous omission an executor could make, so it comes first. The looper detects Track 1 + Track 4 as its own raw ADC band, 1280 to 1390, and checks it *before* `decode_tracks`, with the comment "so the combo isn't mistaken for a Track-4 press" (`main.c:6964-6981`).

That band sits inside this firmware's track-4 range (950 to 1500, Task 2.1 Step 6). Track 4 is preset B. So **without this check, holding the documented recovery combo for two seconds fires `BTN_EV_HOLD` on preset B: it writes flash, destroys the stored scene, and never enters DFU.** The one gesture that is supposed to rescue a wedged puck instead becomes a destructive write on scarce hardware.

```c
/* FAILSAFE, transplanted from sp1-tape-looper main.c:6964-6981 (MIT).
 * Track 1 + Track 4 together read as a distinct ADC band (~1325, between
 * track 4 at ~1220 and play at ~1823). Held 1.2 s it resets into the
 * bootloader. Checked BEFORE decode: the band lies inside track 4's range,
 * so decoding first would turn the recovery gesture into a preset save.
 * Time-based rather than a per-iteration counter so a slow control pass
 * cannot skew the threshold. */
#define COMBO14_LO      1280
#define COMBO14_HI      1390
#define COMBO14_HOLD_MS 1200

static int debounced_track_button(void)
{
    static int committed = -1, candidate = -1, count;
    static int64_t combo14_t = -1;
    static int err_run;

    int raw = board_io_read_track_ladder();
    if (raw < 0) {
        /* Holding the committed value forever on ADC failure is how a
         * pressed button becomes a phantom press that toggles freeze on
         * its own. Tolerate a few bad reads, then report nothing pressed. */
        if (++err_run < 10) {
            return committed;
        }
        committed = -1;
        combo14_t = -1;
        return committed;
    }
    err_run = 0;

    if (raw >= COMBO14_LO && raw <= COMBO14_HI) {
        if (combo14_t < 0) {
            combo14_t = k_uptime_get();
        } else if (k_uptime_get() - combo14_t >= COMBO14_HOLD_MS) {
            board_io_enter_dfu();          /* does not return */
        }
        committed = -1;                    /* never a button while in-band */
        candidate = -1;
        count     = 0;
        return committed;
    }
    combo14_t = -1;
    ...
}
```

Expose `board_io_enter_dfu()` from `board_io.c` (transplanted from `main.c:5744-5753`) and add it to `board_io.h`. Add to Task 2.1's interface list.

Note this makes the combo unavailable as a normal chord, which is fine: the surface has no chords (see Step 3).

- [ ] **Step 2: Add sticky debounce for the shared ladder**

The track buttons sit on one noisy resistor ladder, so a single raw read at a band boundary can name the wrong button. The looper commits a new value only after three consecutive agreeing reads (`main.c:7513-7519`). Do the same:

```c
/* Three agreeing passes (15 ms) before a ladder reading is believed. A
 * finger transiting the ladder cannot fire a neighbouring button. */
static int debounced_track_button(void)
{
    static int committed = -1, candidate = -1, count;
    /* (the combo and ADC-error handling from Step 1 sit here) */
    int b = board_io_decode_track_button(raw);
    if (b == committed) {
        count = 0;
    } else if (b == candidate) {
        if (++count >= 3) { committed = b; count = 0; }
    } else {
        candidate = b; count = 1;
    }
    return committed;
}
```

- [ ] **Step 3: Feed the engine from the control loop**

Add `#include "buttons.h"` to `main.c`, declare the engine next to `g_fader`, and initialise it before the loop:

```c
static button_engine_t eng;
static btn_state_t     g_btn[PROFILE_NUM_BUTTONS];
```

```c
    button_engine_init(&eng, prof);
```

Then, inside the loop after the fader block:

```c
        uint32_t now     = (uint32_t)k_uptime_get();
        int      pressed = debounced_track_button();  /* -1, 0..3, 4 = play */
        cc_msg_t msgs[PROFILE_MAX_CAPTURE];

        for (int i = 0; i < PROFILE_NUM_BUTTONS; i++) {
            uint8_t ev = btn_update(&g_btn[i], pressed == i, now);
            if (!ev) {
                continue;
            }
            int n = button_engine_event(&eng, i, ev, msgs,
                                        (int)(sizeof(msgs) / sizeof(msgs[0])));
            if (n == BUTTON_ACTION_SAVE_PRESET) {
                /* TODO(Phase 7): snapshot the surface and persist it. */
                continue;
            }
            for (int k = 0; k < n; k++) {
                midi_tx_send(msgs[k]);
            }
        }
```

No duration is threaded through, and there is no periodic tick. That is the whole benefit of the toggle decision: an earlier design timed the release to offer a momentary pedal, which needed the press duration handed across from `btn_update` (easy to get wrong: reading it afterwards returns zero, because the release has already cleared the timer, which silently made every hold behave as a tap) plus a timeout to rescue a release that never arrived. None of that exists now. Freeze advances on presses only.

Queuing the whole burst at once is deliberate: the drain thread paces it on the wire, so the control loop stays at 5 ms even during a preset recall. This is the reason the queue exists.

Only one ladder button can be read at a time by construction, so simultaneous presses are not supported. That matches the spec's surface, which has no chords.

- [ ] **Step 4: Build, flash, and test at the rack**

Expected, in this order:
1. Press track 1: freeze engages. Press again: it releases. It should feel immediate, because it acts on the press.
2. Hold track 1 down for several seconds and let go: **nothing happens on release**, freeze simply stays on. That is correct now, and it is the property that makes a lost release harmless.
3. Press track 2 four times: shimmer steps through its four states and returns to the start.
4. Press track 3 or 4 briefly: nothing yet (no preset stored).
5. **Verify the recovery combo actually recovers.** Hold Track 1 + Track 4 together for about 1.5 s with the app running. Expected: all four track LEDs light and the puck resets into the bootloader. Then confirm preset B was NOT overwritten. If instead the preset-save blink fires, Step 1 is missing or the band is wrong on this unit, and you have just lost your recovery path.
6. Hammer it: fast repeated presses, two buttons at once, presses during a fader sweep. Confirm the freeze LED and the audible state never disagree. A toggle that misses a press shows up here as an inverted state, and the fix is one more press, but you want to know how often it happens.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/main.c
git commit -m "feat: track buttons drive freeze and shimmer, recovery combo wired"
```

---

# Phase 6: LEDs as readout

### Task 6.1: Panel feedback

**Files:**
- Modify: `firmware/src/main.c`
- Modify: `firmware/src/board_io.c` (only if Task 2.3 found brightness unavailable)

An honest framing, per the revised spec: these LEDs show the **puck's own state**, which equals the synth's state only while the takeover policy holds. MIDI here is one-way and there is no readback, so nothing on this panel can prove the other end is listening.

The bounded consequence, worth knowing before it confuses you at the rack: if the puck reboots while the synth is frozen, the puck comes up believing freeze is off and stays silent, so unfreezing takes two presses with a wrong LED in between. If the synth is power-cycled instead, the mirror case applies, and shimmer needs at most four presses to re-agree. No design without readback can do better than this, and every case self-corrects within one full cycle of the control.

- [ ] **Step 1: Mirror fader values on the centre row**

```c
        for (int i = 0; i < PROFILE_NUM_FADERS; i++) {
            uint8_t level = g_fader[i].have_sent
                          ? (uint8_t)((g_fader[i].last_sent * 255) / 127)
                          : 0u;
            board_io_led_set(i, level);
        }
```

If Task 2.3 established that the LEDs are on/off only, render instead as lit above 64 plus a short blink on change, and record which rendering you implemented in `docs/hardware-notes.md`.

- [ ] **Step 2: Show pickup state**

A fader in soft pickup is not in control yet, and the performer needs to know which way to move. Blink that fader's LED while `fader_pickup_armed(&g_fader[i])` is true, rather than showing a steady level. It stops blinking the instant the fader catches its value, which is also the confirmation that it took over.

- [ ] **Step 3: Mirror button state on the track row**

```c
        board_io_track_led_set(0, button_engine_is_latched(&eng, 0));
        board_io_track_led_set(1, button_engine_step_index(&eng, 1) != 0);
        for (int b = 2; b < PROFILE_NUM_BUTTONS; b++) {
            const preset_slot_t *p =
                button_engine_get_preset(&eng, prof->button[b].preset_slot);
            board_io_track_led_set(b, p && p->len > 0);
        }
```

REVIEW: `board_io_track_led_set` is boolean, so as written this shows only "shimmer is not off", and the four-level reading described in an earlier draft of Task 2.3 is not achievable through this API. Either accept the boolean reading (fine for v1) or, if Task 2.3 found the track LEDs accept soft-PWM, widen the API to take a level and render four brightnesses. Decide from the survey and record which you did. Do not leave the hardware expectation and the API disagreeing.

- [ ] **Step 4: Local activity indicator, not a link heartbeat**

Blink one LED briefly on each transmission. Call it what it is in the code comment: a local TX indicator. It proves the puck sent something, not that anything received it.

- [ ] **Step 5: Verify on hardware**

Expected: moving a fader visibly changes its LED; freeze shows as a lit track LED and clears on the **second press** (releasing the button does nothing, which is the point of the toggle); shimmer brightness or state steps with each tap; a fader in pickup blinks until it catches. Nothing flickers when the puck is left alone: a flickering idle LED means the fader deadband is letting jitter through and Task 2.2's measurement needs revisiting.

- [ ] **Step 6: Commit**

```bash
git add firmware/src docs/hardware-notes.md
git commit -m "feat: LED readout for fader values, pickup state and buttons"
```

---

# Phase 7: Presets

### Task 7.0: Prove the storage page is ours before writing to it

REVIEW raised this as a device-recovery hazard, and it is the right call. Both reference board files *label* `0xFF000` as `storage`, but neither demonstrates that the TE bootloader ignores it. If the bootloader keeps settings, image-validation state or recovery metadata there, `flash_area_erase` on that page is a plausible soft-brick on scarce hardware. A label in a devicetree written by the community is a hypothesis, not a fact.

**Files:**
- Modify: `firmware/src/main.c` (temporary diagnostic, reverted at the end)
- Modify: `docs/hardware-notes.md`

- [ ] **Step 1: Dump the page, read only**

Behind `SP1_DIAG`, print the full 4 KB at `0xFF000` as hex over the console. Read only: no erase, no write.

**Do not print it at boot.** A CDC-ACM console throws away everything written before the host opens the port, so a one-shot boot dump is usually invisible, and this dump gates a STOP rule. Trigger it on a button press instead (any track button will do, the profile is not wired up yet at this point in the plan), or gate it on DTR being asserted.

```c
        const struct flash_area *fa;
        if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa) == 0) {
            static uint8_t page[4096];
            if (flash_area_read(fa, 0, page, sizeof(page)) == 0) {
                for (int i = 0; i < 4096; i += 16) {
                    printk("%04x:", i);
                    for (int j = 0; j < 16; j++) printk(" %02x", page[i + j]);
                    printk("\n");
                }
            }
            flash_area_close(fa);
        }
```

- [ ] **Step 2: Interpret what came back**

- **All `0xFF`:** the page has never been written. It is almost certainly free, and nothing we do can destroy state that is not there. Proceed, and note that the first erase will not happen until 102 saves in.
- **Structured non-`0xFF` content:** something owns this page. **Stop.** Do not write. Record the dump in `docs/hardware-notes.md` and re-plan: the fallback is to keep presets in RAM only for v1 (they survive a power cycle only if the puck stays charged, which the session test can still evaluate) and to ask on the Discord what lives there.
- **A few scattered non-`0xFF` bytes:** ambiguous. Treat as owned until proved otherwise.

- [ ] **Step 3: Confirm the flasher really does stop below it**

Note the current dump, reflash the firmware via the utility, dump again. If the content is unchanged, the claim from `protocol.js:10-11` (that writing stops at `0xFF000`) is confirmed on real hardware rather than read off a constant.

- [ ] **Step 4: Record the verdict and commit**

```bash
git add docs/hardware-notes.md firmware/src/main.c
git commit -m "docs: storage page at 0xFF000 dumped and proved free before use"
```

**STOP RULE:** Phase 7 does not proceed to writing until this task says the page is free.

---

### Task 7.1: Preset record format, pure logic

**Files:**
- Create: `firmware/src/presets.h`
- Create: `firmware/src/presets.c`
- Create: `tests/host/test_presets.c`
- Modify: `tests/host/Makefile`

**Interfaces:**
- Produces:
  - `#define PRESET_REC_SIZE 40`
  - `void preset_record_encode(const preset_bank_t *bank, uint8_t rec[PRESET_REC_SIZE]);`
  - `bool preset_record_decode(const uint8_t rec[PRESET_REC_SIZE], preset_bank_t *out);`
  - `int  preset_page_find_latest(const uint8_t *page, uint32_t page_len, preset_bank_t *out);`
  - `int  preset_page_next_offset(const uint8_t *page, uint32_t page_len);`

Storage design, and why: the flasher writes `0x20000` to `0xFF000` and never beyond, so the 4 KB page at `0xFF000` survives reflashing. That is exactly one flash page, and Zephyr NVS needs at least two sectors, so NVS is out. Instead the page is an append-log of fixed 40-byte records. A save writes the next erased slot; reading takes the last valid record; when the page fills, erase it once and start again. 102 saves per erase cycle, which on this hardware is effectively unlimited.

- [ ] **Step 1: Write the failing test, `tests/host/test_presets.c`**

```c
#include <string.h>
#include "presets.h"
#include "test_util.h"

#define PAGE_LEN 4096

static uint8_t page[PAGE_LEN];

static void erase_page(void) { memset(page, 0xFF, sizeof(page)); }

static preset_bank_t make_bank(uint8_t seed)
{
    preset_bank_t b;
    memset(&b, 0, sizeof(b));
    b.slot[0].len = 2;
    b.slot[0].msg[0] = (cc_msg_t){ 0, 102, (uint8_t)(seed + 1) };
    b.slot[0].msg[1] = (cc_msg_t){ 0, 104, (uint8_t)(seed + 2) };
    b.slot[1].len = 1;
    b.slot[1].msg[0] = (cc_msg_t){ 0, 107, (uint8_t)(seed + 3) };
    return b;
}

static void test_round_trip(void)
{
    uint8_t rec[PRESET_REC_SIZE];
    preset_bank_t in = make_bank(10), out;
    preset_record_encode(&in, rec);
    CHECK(preset_record_decode(rec, &out));
    CHECK_EQ(out.slot[0].len, 2);
    CHECK_EQ(out.slot[0].msg[1].cc, 104);
    CHECK_EQ(out.slot[0].msg[1].value, 12);
    CHECK_EQ(out.slot[1].msg[0].value, 13);
}

static void test_corrupt_record_is_rejected(void)
{
    uint8_t rec[PRESET_REC_SIZE];
    preset_bank_t in = make_bank(10), out;
    preset_record_encode(&in, rec);
    rec[5] ^= 0xFF;                       /* flip a payload bit */
    CHECK(!preset_record_decode(rec, &out));
}

static void test_erased_record_is_rejected(void)
{
    uint8_t rec[PRESET_REC_SIZE];
    preset_bank_t out;
    memset(rec, 0xFF, sizeof(rec));
    CHECK(!preset_record_decode(rec, &out));
}

static void test_empty_page_has_no_latest_and_starts_at_zero(void)
{
    erase_page();
    preset_bank_t out;
    CHECK_EQ(preset_page_find_latest(page, PAGE_LEN, &out), -1);
    CHECK_EQ(preset_page_next_offset(page, PAGE_LEN), 0);
}

static void test_latest_wins(void)
{
    erase_page();
    preset_bank_t a = make_bank(1), b = make_bank(50), out;
    preset_record_encode(&a, page + 0);
    preset_record_encode(&b, page + PRESET_REC_SIZE);
    CHECK_EQ(preset_page_find_latest(page, PAGE_LEN, &out), 1);
    CHECK_EQ(out.slot[0].msg[0].value, 51);
    CHECK_EQ(preset_page_next_offset(page, PAGE_LEN), 2 * PRESET_REC_SIZE);
}

static void test_corrupt_tail_falls_back_to_the_last_good_record(void)
{
    erase_page();
    preset_bank_t a = make_bank(1), out;
    preset_record_encode(&a, page + 0);
    preset_record_encode(&a, page + PRESET_REC_SIZE);
    page[PRESET_REC_SIZE + 4] ^= 0xFF;    /* the newer record is damaged */
    CHECK_EQ(preset_page_find_latest(page, PAGE_LEN, &out), 0);
    CHECK_EQ(out.slot[0].msg[0].value, 2);
}

static void test_full_page_reports_no_room(void)
{
    erase_page();
    preset_bank_t a = make_bank(1);
    for (uint32_t off = 0; off + PRESET_REC_SIZE <= PAGE_LEN;
         off += PRESET_REC_SIZE) {
        preset_record_encode(&a, page + off);
    }
    CHECK_EQ(preset_page_next_offset(page, PAGE_LEN), -1);
}

int main(void)
{
    RUN(test_round_trip);
    RUN(test_corrupt_record_is_rejected);
    RUN(test_erased_record_is_rejected);
    RUN(test_empty_page_has_no_latest_and_starts_at_zero);
    RUN(test_latest_wins);
    RUN(test_corrupt_tail_falls_back_to_the_last_good_record);
    RUN(test_full_page_reports_no_room);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run and watch it fail**

```bash
sed -i '' 's/^TESTS := smoke txqueue controls profile buttons/TESTS := smoke txqueue controls profile buttons presets/' tests/host/Makefile
make -C tests/host test
```
Expected: FAIL, `presets.h` not found.

- [ ] **Step 3: Write `firmware/src/presets.h`**

```c
/* PURE. Preset records for the single 4 KB flash page at 0xFF000, which
 * the firmware flasher never erases (it stops at 0xFF000, see
 * solderless utility/js/protocol.js:10-11). One page is not enough for
 * Zephyr NVS, so the page is an append-log of fixed-size records: write
 * the next erased slot, read the last valid one, erase only when full. */
#ifndef SP1_PRESETS_H
#define SP1_PRESETS_H

#include <stdbool.h>
#include <stdint.h>
#include "buttons.h"

#define PRESET_REC_SIZE   40
#define PRESET_MAGIC_0    0x53   /* 'S' */
#define PRESET_MAGIC_1    0x50   /* 'P' */
#define PRESET_VERSION    1

typedef struct {
    preset_slot_t slot[BUTTON_MAX_PRESET_SLOTS];
} preset_bank_t;

/* Layout, 40 bytes:
 *   [0]      magic 'S'
 *   [1]      magic 'P'
 *   [2]      version
 *   [3]      reserved (0)
 *   [4]      slot 0 length
 *   [5..19]  slot 0, up to 5 entries of (channel, cc, value)
 *   [20]     slot 1 length
 *   [21..35] slot 1, up to 5 entries
 *   [36..38] reserved (0)
 *   [39]     CRC-8 over bytes 0 to 38 */
#define PRESET_ENTRIES 5

#if PROFILE_MAX_CAPTURE > PRESET_ENTRIES
#error "preset records cannot hold the profile's capture list"
#endif

void preset_record_encode(const preset_bank_t *bank, uint8_t rec[PRESET_REC_SIZE]);
bool preset_record_decode(const uint8_t rec[PRESET_REC_SIZE], preset_bank_t *out);

/* Index of the newest valid record, or -1 when the page holds none. */
int  preset_page_find_latest(const uint8_t *page, uint32_t page_len,
                             preset_bank_t *out);

/* Byte offset of the next writable slot, or -1 when the page is full. */
int  preset_page_next_offset(const uint8_t *page, uint32_t page_len);

#endif /* SP1_PRESETS_H */
```

Both constants are 5 today, so the guard is silent. It exists so that a future profile with a longer capture list fails the build instead of quietly losing the tail of a scene.

- [ ] **Step 4: Write `firmware/src/presets.c`**

```c
#include <string.h>
#include "presets.h"

static uint8_t crc8(const uint8_t *p, uint32_t len)
{
    uint8_t crc = 0x00;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (uint8_t)((crc & 0x80u) ? ((crc << 1) ^ 0x07u) : (crc << 1));
        }
    }
    return crc;
}

void preset_record_encode(const preset_bank_t *bank, uint8_t rec[PRESET_REC_SIZE])
{
    memset(rec, 0, PRESET_REC_SIZE);
    rec[0] = PRESET_MAGIC_0;
    rec[1] = PRESET_MAGIC_1;
    rec[2] = PRESET_VERSION;

    uint32_t o = 4;
    for (int s = 0; s < BUTTON_MAX_PRESET_SLOTS; s++) {
        uint8_t len = bank->slot[s].len;
        if (len > PRESET_ENTRIES) {
            len = PRESET_ENTRIES;
        }
        rec[o++] = len;
        for (int i = 0; i < PRESET_ENTRIES; i++) {
            if (i < len) {
                rec[o++] = bank->slot[s].msg[i].channel;
                rec[o++] = bank->slot[s].msg[i].cc;
                rec[o++] = bank->slot[s].msg[i].value;
            } else {
                o += 3;
            }
        }
    }
    rec[PRESET_REC_SIZE - 1] = crc8(rec, PRESET_REC_SIZE - 1);
}

bool preset_record_decode(const uint8_t rec[PRESET_REC_SIZE], preset_bank_t *out)
{
    if (rec[0] != PRESET_MAGIC_0 || rec[1] != PRESET_MAGIC_1 ||
        rec[2] != PRESET_VERSION) {
        return false;
    }
    if (crc8(rec, PRESET_REC_SIZE - 1) != rec[PRESET_REC_SIZE - 1]) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    uint32_t o = 4;
    for (int s = 0; s < BUTTON_MAX_PRESET_SLOTS; s++) {
        uint8_t len = rec[o++];
        if (len > PRESET_ENTRIES) {
            return false;
        }
        out->slot[s].len = len;
        for (int i = 0; i < PRESET_ENTRIES; i++) {
            if (i < len) {
                out->slot[s].msg[i].channel = rec[o];
                out->slot[s].msg[i].cc      = rec[o + 1];
                out->slot[s].msg[i].value   = rec[o + 2];
            }
            o += 3;
        }
    }
    return true;
}

int preset_page_find_latest(const uint8_t *page, uint32_t page_len,
                            preset_bank_t *out)
{
    int found = -1;
    for (uint32_t off = 0, idx = 0; off + PRESET_REC_SIZE <= page_len;
         off += PRESET_REC_SIZE, idx++) {
        preset_bank_t tmp;
        if (preset_record_decode(page + off, &tmp)) {
            *out  = tmp;
            found = (int)idx;
        }
    }
    return found;
}

int preset_page_next_offset(const uint8_t *page, uint32_t page_len)
{
    for (uint32_t off = 0; off + PRESET_REC_SIZE <= page_len;
         off += PRESET_REC_SIZE) {
        bool erased = true;
        for (uint32_t i = 0; i < PRESET_REC_SIZE; i++) {
            if (page[off + i] != 0xFF) { erased = false; break; }
        }
        if (erased) {
            return (int)off;
        }
    }
    return -1;
}
```

- [ ] **Step 5: Run the tests**

```bash
make -C tests/host test
```
Expected: PASS for all six suites.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/presets.c firmware/src/presets.h tests/host
git commit -m "feat: preset record format with CRC and single-page append-log"
```

---

### Task 7.2: Flash storage, the save gesture, and pickup on recall

**Files:**
- Create: `firmware/src/presets_flash.c`
- Modify: `firmware/src/presets.h` (add the two IO prototypes)
- Modify: `firmware/src/main.c`

**Interfaces:**
- Produces:
  - `bool preset_store_load(preset_bank_t *out);`
  - `bool preset_store_save(const preset_bank_t *bank);`
  - These are the only functions in the preset path that touch Zephyr, so `presets_flash.c` is excluded from `SRC_presets` in the host Makefile.

- [ ] **Step 1: Write `firmware/src/presets_flash.c`**

Use the flash map API against the `storage` partition declared in the board devicetree (`storage_partition` at `0xFF000`, length `0x1000`).

```c
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include "presets.h"

#define STORAGE_PARTITION  storage_partition
#define STORAGE_ID         FIXED_PARTITION_ID(STORAGE_PARTITION)

bool preset_store_load(preset_bank_t *out)
{
    const struct flash_area *fa;
    if (flash_area_open(STORAGE_ID, &fa) != 0) {
        return false;
    }
    static uint8_t page[4096];
    int rc = flash_area_read(fa, 0, page, sizeof(page));
    flash_area_close(fa);
    if (rc != 0) {
        return false;
    }
    return preset_page_find_latest(page, sizeof(page), out) >= 0;
}

bool preset_store_save(const preset_bank_t *bank)
{
    const struct flash_area *fa;
    if (flash_area_open(STORAGE_ID, &fa) != 0) {
        return false;
    }

    static uint8_t page[4096];
    bool ok = false;

    if (flash_area_read(fa, 0, page, sizeof(page)) == 0) {
        int off = preset_page_next_offset(page, sizeof(page));
        if (off < 0) {
            /* The page is full: erase once and start at the beginning.
             * At 102 records per cycle this is a rare event. */
            if (flash_area_erase(fa, 0, sizeof(page)) == 0) {
                off = 0;
            }
        }
        if (off >= 0) {
            uint8_t rec[PRESET_REC_SIZE];
            preset_record_encode(bank, rec);
            /* On nRF52840 a flash word write stalls the CPU for ~41 us and
             * a page erase for ~85 ms. A MIDI bit is 32 us, so a write
             * landing mid-byte can corrupt that byte's framing on the wire
             * (the receiver resyncs at the next status byte) and will
             * visibly stutter the LED soft-PWM. Saving is a deliberate 2 s
             * gesture and rarely overlaps a sweep, so this is a nuisance
             * rather than a fault, but if it shows up, let the transmit
             * queue drain before writing. */
            ok = (flash_area_write(fa, (off_t)off, rec, sizeof(rec)) == 0);
        }
    }

    flash_area_close(fa);
    return ok;
}
```

Add both prototypes to `presets.h` under a comment marking them as the Zephyr-only part of the module.

- [ ] **Step 2: Load presets at boot in `main.c`**

After `button_engine_init`, call `preset_store_load` and, on success, push each slot into the engine with `button_engine_set_preset`. Loading must not transmit anything: the silent-boot rule still holds.

- [ ] **Step 3: Handle the save action**

Replace the `TODO(Phase 7)` branch:

```c
/* Snapshot what the puck last SENT, which is all it knows: MIDI here is
 * one-way, so tweaks made from Push 3 or the synth's own menu are not
 * captured. Accepted in the design. Freeze is deliberately excluded by
 * the profile's capture list. */
static int snapshot_surface(const profile_t *prof, cc_msg_t *out, int out_max)
{
    int n = 0;
    for (int c = 0; c < prof->preset_capture_len; c++) {
        uint8_t cc = prof->preset_capture[c];
        /* REVIEW: every one of these writes needs its OWN bound check, not
         * just the outer loop. A generic profile may map the same CC to two
         * faders, and then one capture entry produces two messages: the
         * outer check would let the second one run off the end. */
        for (int f = 0; f < PROFILE_NUM_FADERS && n < out_max; f++) {
            if (prof->fader[f].cc == cc && g_fader[f].have_sent) {
                out[n++] = (cc_msg_t){ prof->fader[f].channel, cc,
                                       g_fader[f].last_sent };
            }
        }
        for (int b = 0; b < PROFILE_NUM_BUTTONS && n < out_max; b++) {
            const button_cfg_t *cfg = &prof->button[b];
            if (cfg->mode == BTN_MODE_CYCLE && cfg->cc == cc) {
                out[n++] = (cc_msg_t){ cfg->channel, cc,
                                       cfg->steps[button_engine_step_index(&eng, b)] };
            }
        }
        if (n >= out_max) {
            break;
        }
    }
    return n;
}
```

Then `button_engine_set_preset(&eng, slot, snap, n)` and persist. REVIEW: `preset_store_save` takes a `preset_bank_t`, i.e. *both* slots, and the engine holds them separately, so the bank has to be assembled first. Missing this would have saved one slot and silently blanked the other:

```c
static bool persist_all_slots(void)
{
    preset_bank_t bank;
    memset(&bank, 0, sizeof(bank));
    for (int s = 0; s < BUTTON_MAX_PRESET_SLOTS; s++) {
        const preset_slot_t *p = button_engine_get_preset(&eng, s);
        if (p) {
            bank.slot[s] = *p;
        }
    }
    return preset_store_save(&bank);
}
```

Then confirm on the LEDs: blink that button's track LED three times fast. A failed write must blink differently (a single long blink) so a silent failure is impossible to miss.

- [ ] **Step 4: Arm soft pickup on every recall**

This is the takeover policy, and it is the one place the puck knowingly desyncs its own faders from what it just sent. After queueing a preset's messages, arm pickup on every fader whose CC was in that burst:

```c
static void resync_after_recall(const profile_t *prof,
                                const cc_msg_t *msgs, int n)
{
    for (int m = 0; m < n; m++) {
        for (int f = 0; f < PROFILE_NUM_FADERS; f++) {
            if (prof->fader[f].cc == msgs[m].cc &&
                prof->fader[f].channel == msgs[m].channel) {
                fader_arm_pickup(&g_fader[f], msgs[m].value);
            }
        }
        /* Cycle buttons need it too. A recall replays CC 105, which moves
         * shimmer on the synth; without this the puck's step index, its
         * LED, and the next press all disagree with what it just sent, and
         * the next snapshot captures the stale step. */
        for (int b = 0; b < PROFILE_NUM_BUTTONS; b++) {
            if (prof->button[b].mode == BTN_MODE_CYCLE &&
                prof->button[b].cc == msgs[m].cc &&
                prof->button[b].channel == msgs[m].channel) {
                button_engine_sync_cycle(&eng, b, msgs[m].value);
            }
        }
    }
}
```

Call it immediately after the loop that queues the messages, in the same branch. Without the fader half, the first ADC pass after a recall sees the fader sitting where it was, decides that differs from the recalled value, and instantly undoes the whole scene. Without the cycle half, shimmer's LED lies and the next press jumps.

- [ ] **Step 5: Build, flash and test the full gesture**

Expected sequence at the rack:
1. Set the four faders somewhere musical, set shimmer to step 2.
2. Hold track 3 for two seconds: the LED confirms with three fast blinks.
3. Move every fader somewhere else.
4. Tap track 3: all four parameters glide back to the stored scene (the synth's own CC smoothing turns the burst into a morph, which is the whole reason presets are CC bursts rather than a new message type).
5. **Immediately after the recall, the faders are in pickup:** their LEDs blink, and moving one does nothing until it crosses the recalled value, at which point it catches and its LED goes steady. Test this deliberately: it is the difference between a usable recall and one that is destroyed by the first accidental nudge.
6. Power the puck off, power it back on, tap track 3 again: the same scene replays. This is the persistence test.
7. Reflash the firmware, power on, tap track 3: the scene is still there, because the flasher stops below `0xFF000`. Confirm it.

- [ ] **Step 6: Commit**

```bash
git add firmware/src
git commit -m "feat: presets persist to flash, recall arms soft pickup"
```

---

# Phase 8: Session validation and release

### Task 8.1: The session that decides the stop rule

**Files:**
- Create: `docs/session-log.md`
- Modify: `README.md`

The spec's stop rule: if after one real session the puck does not beat Push 3 for performing these four gestures, the firmware is parked and the puck returns to stock. This task executes that test honestly.

- [ ] **Step 1: Play one real rack session**

String synth, puck on the desk, Push 3 pushed out of reach (the takeover policy says the puck owns these four CCs during a performance, so do not ride them from both). Ride cutoff, reverb wet, delay time and delay feedback from the faders. Toggle freeze on and off at least twice. Step shimmer at least once. Store and recall both presets.

- [ ] **Step 2: Write down what actually happened**

In `docs/session-log.md`: what worked, what felt wrong, what was reached for and not found, any stuck or missed message, whether fader resolution felt sufficient at 7 bits, whether the pickup behaviour after a recall felt natural or fiddly, and **whether you reached for a momentary freeze stab and found only a toggle**. That last one is the specific thing the toggle decision traded away, so it is the specific thing to watch for. Be specific. Vague notes here waste the next session.

- [ ] **Step 3: Make the call**

Either: v1 is proven, record it in `README.md` under status, and list any tuning changes worth making. Or: the stop rule fires, record why, and reflash the dev puck to stock or the looper. Both outcomes are legitimate results of the plan and neither is a failure of it.

- [ ] **Step 4: Commit**

```bash
git add docs/session-log.md README.md
git commit -m "docs: first rack session, stop-rule decision recorded"
```

---

### Task 8.2: Release artefact

**Files:**
- Create: `firmware/release/sp1-remote-v1.bin`
- Modify: `README.md`

- [ ] **Step 1: Build a clean release image with diagnostics compiled out**

```bash
# set SP1_DIAG to 0 in main.c first
west build -p -b stem_player firmware -- -DBOARD_ROOT=$(pwd)
mkdir -p firmware/release
cp build/zephyr/zephyr.bin firmware/release/sp1-remote-v1.bin
ls -l firmware/release/sp1-remote-v1.bin
```

- [ ] **Step 2: Flash the release image and re-run the smoke test**

Silent at boot, faders move their parameters, freeze works both ways, shimmer steps, presets recall and arm pickup. Do not skip this: the last build tested is the one that ships.

- [ ] **Step 3: Finish `README.md`**

Add: the flashing instructions pointing at `docs/flashing.md`, the recovery procedure, the default profile table, a "how to change the mapping" section pointing at `firmware/src/profile.c`, and a credits section naming chattock, marcabisamra, timknapen, ericlewis and softmodded.

- [ ] **Step 4: Tag and push**

```bash
git add firmware/release README.md
git commit -m "release: sp1-remote v1"
git tag -a v1.0 -m "SP-1 Remote v1: four faders, four buttons, TRS MIDI out"
git push origin main --tags
```

---

## Parked for later (not in this plan)

- **v1.1 WebSerial profile editor.** A browser page that rewrites the profile table over the CDC console: any CC, any channel, per control. Proven feasible on this hardware by the solderless flasher and by feldd's browser remapping. This is what makes the firmware genuinely generic rather than PopGoblin-shaped.
  **Blocked on storage, not on the UI.** `profile_popgoblin_default` is `const`, compiled into the app image at `0x20000`, which the flasher overwrites on every firmware update. A runtime-editable profile has to live in the storage page at `0xFF000`, and that is the only 4 KB page there is, already holding the preset append-log. The sizes fit comfortably (the profile is roughly 140 bytes, preset records are 40), so they can share the page, but it needs a combined layout and it depends on **Task 7.0 proving the page is ours at all**. If Task 7.0 finds the bootloader owns it, presets and the editor have to be rethought together. Settle that before starting the browser page, or you will build the fun part and discover the boring blocker halfway in.
- **MIDI clock output.** The transplanted TX already carries the code path; it needs a tempo source. Only worth it if the synth-side delay clock-sync work lands.
- **Momentary freeze (press-and-lift) as a per-button option.** Traded away for v1 in favour of a plain toggle, because it made the release load-bearing. If the first session shows you reaching for sub-second stabs, it comes back as a third button mode rather than as the default, so the robust behaviour stays the one you get by accident.
- **Function button as a shift layer** for a second CC bank, which would give palette cycling (CC 43) a home. YAGNI until v1 has been performed with.
- **Renode emulation** via `softmodded/spire`, for firmware iteration without touching scarce hardware. Worth standing up only if hardware access becomes the bottleneck.

## Risks carried into execution

- **The TRS MIDI TX is not proven in writing, and the sync jack is undocumented.** Mitigated by Phase 0 preceding the bench, by Task 1.2 measuring source resistance and drive current before anything is connected to the Tiliqua, and by Task 1.3's decision tree.
- **The pucks are unreleased prototypes.** Mitigated by developing on one, by the recovery drill in Task 1.1, and by the BIG FIVE constraints being restated at the top of `main.c`.
- **USB interrupt activity jittering the bit-banged TRS bit edges.** A 32 us bit driven from a timer ISR does not have much margin, and USB adds an interrupt every millisecond. Mitigated by the explicit soak test in Task 3.3 Step 5, and by the looper having run much heavier isochronous USB audio against the same transmitter. If it bites, USB becomes a bench-only build.
- **Touch-fader jitter becoming CC zipper.** Mitigated by sizing the deadband from measured jitter in Task 2.2, by the coalescing queue, and by the synth's own CC smoothing. The idle-flicker check in Task 6.1 is the canary.
- **The recovery combo colliding with a real button.** Track 1 + Track 4 sits inside track 4's ADC band, so decode order is load-bearing: get it wrong and the rescue gesture becomes a flash write. Mitigated by the pre-decode band check in Task 5.2 Step 1 and by the explicit rack test that the combo reaches DFU without touching preset B. This was found only on the second review pass, after the first had already "fixed" the missing hatch.
- **~~A missed button release leaving freeze stuck on.~~ Designed out.** This was the single most session-ruining failure in the surface, and the toggle decision removes the mechanism rather than mitigating it: release is inert, so there is nothing for a lost release to strand. What remains is a much milder failure, a missed *press* leaving the puck's idea of freeze inverted from the synth's, which costs one extra press and re-syncs by itself. The bounded ADC-error run in `debounced_track_button` still matters, because a phantom press would toggle freeze spuriously.
- **A recall being undone by the first fader nudge.** Mitigated by soft pickup (Task 7.2 Step 4) and made visible by the blinking LED (Task 6.1 Step 2).
