# SP-1 Remote Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn one Teenage Engineering SP-1 into a generic 4-fader / 4-button TRS-MIDI controller whose shipped default profile drives the PopGoblin string synth's existing CC map.

**Architecture:** A small Zephyr application for the nRF52840, built against the board definition vendored from `chattock/sp1-tape-looper` (MIT). Hardware-facing code (ADC ladders, LEDs, watchdog, power, bit-banged TRS MIDI TX) is lifted from that firmware, which carries hardware-verified constants. All decision logic (fader conditioning, button state machines, the profile table, preset serialisation) lives in Zephyr-free C files that compile unchanged into a host test binary, so the logic is unit-tested on the Mac and only the electrical behaviour needs the bench.

**Tech Stack:** Zephyr RTOS v4.3.1, Zephyr SDK 0.17.4, CMake/Ninja/west, C11, host tests in plain C11 with clang, flashing via the local `solderless.engineering` mirror over WebSerial in Chrome.

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

Electrical facts for the adapter: MIDI data leaves on the **ring** of the sync jack, driven by P0.23 (BC807 base) through a PNP that inverts (main.c:4405-4410). The tip carries Pocket-Operator pulses (P0.20 / P0.17). The Tiliqua input is a standard opto-isolated TRS-A stage: `H11L1SR2M` with 220R in series (hardware/schematics/tiliqua-motherboard-r5.1.pdf, sheet `midi`). At 3.3 V through 220R that is roughly 8-9 mA into the opto LED, comfortably inside its rating, so no extra series resistor is needed if the SP-1 sources 3.3 V. The adapter is therefore SP-1 ring to Tiliqua ring, SP-1 sleeve to Tiliqua tip, SP-1 tip unconnected. This is a hypothesis to confirm with a multimeter in Task 1.2, not a fact.

**3. The synth is omni, and CC 64 is level-based, not a toggle.**
`gateware/src/top/popgoblin/fw/src/main.rs:115-124` parses `MidiMessage::ControlChange(_, cc, val)` and discards the channel, so any channel works (pre-flight #4 answered from source). CC 64 sets `cc64_held = v >= 64`, i.e. the synth holds freeze for as long as the last value it saw was 64 or more. The spec's "tap = latch, hold = momentary" is therefore implemented entirely puck-side, which is what Phase 5 does.

**Firmware base: the spec's option A, executed as a lift rather than a fork.**
The spec left the choice open until pre-flight (A: fork `sp1-tape-looper`; B: build on the `sp1-midi` Zephyr template). Having read both, the plan takes A's code and B's shape: a new, small application that vendors the looper's board definition and lifts its hardware-facing routines, each with an attribution comment naming the source line range.

Why not a straight fork: `firmware/src/main.c` is 7614 lines, and roughly nine tenths of it is the looper engine, the eMMC driver and the I2S audio path, all interlocked through shared volatile globals. Deleting that safely is harder and riskier than lifting the 600 or so lines that matter.

Why not B: `sp1-midi` is a genuinely nice BSP and its `MidiController` already does fader-to-CC (`app/MidiController.hpp:14-26`, four faders, a deadband of 8), but it sends over **USB MIDI 2** (`usbd_midi2.h`), not over the TRS jack. The one thing this project cannot get anywhere else is the looper's bit-banged TRS transmitter. Its board files also declare PWM LED nodes that would fight direct GPIO writes, which is exactly why the looper's own board file drops them. Keep `sp1-midi` checked out as a structural reference.

If the lift stalls, the fallback is a straight fork of the looper with the engine deleted. Decide that at Task 2.1, on evidence, not in advance.

**One scope correction:** the spec says "fader LED trails mirror the last-sent value". The known pin map has eight discrete LEDs, not per-fader trails: four centre-row LEDs (main.c:101-104) and four track LEDs above the buttons (main.c:107-110). There is no evidence of an addressable trail. Phase 6 therefore renders fader value as *brightness* on the centre-row LED via the existing soft-PWM renderer, and uses the track LEDs for button state. Task 2.3 includes a short LED survey to confirm there is nothing else on the panel; if a trail turns up, Phase 6 grows a task.

---

## Global Constraints

- **Develop on ONE puck only.** The other pucks stay stock until v1 is proven. Mark the dev puck physically before Phase 1.
- **The SP-1 "BIG FIVE" bootloader rules are non-negotiable** (source: `sp1-tape-looper/firmware/src/main.c:40-44`). The app lives at `0x20000`; the watchdog is fed at least every 5 s; bootloader-owned clocks and peripherals are not re-initialised; `SYSTEM_OFF` is the only power-down path; `RESETREAS` is cleared at boot and again before `SYSTEM_OFF`. There is no hardware reset pin on the SP-1. A firmware that hangs without feeding the watchdog, or that cannot get back to the bootloader, is a brick.
- **Bootloader entry:** power off, hold Track 1 + Track 4, plug in USB-C, release once the Track 1 LED lights.
- **Flash image format:** raw `.bin`, written to `0x20000`, maximum size `0xDF000` (`solderless/utility/js/protocol.js:8-13`). The flasher never touches `0xFF000` and above, so the 4 KB storage partition survives a reflash. Preset data must live there and nowhere else.
- **Only one 4 KB page of storage exists** (`0xFF000` to `0x100000`). Zephyr NVS needs two sectors, so it is not usable. Presets use an append-log inside the single page (Phase 7).
- **No changes to the synth.** Not gateware, not firmware. The popgoblin CC map is the entire contract.
- **Fader full-scale raw code is 3700** on the 12-bit SAADC as configured (gain 1/6, 0.6 V internal reference, 20 us acquisition). Source: `main.c:7487`.
- **CC emission:** 7-bit, send-on-change only, at most one message per 10 ms per fader. Multi-message bursts are spaced 1 to 2 ms.
- **Default MIDI channel is 1** (wire value 0). Per-control channel is configurable in the profile table.
- **Zephyr v4.3.1 with SDK 0.17.4.** Pinned; the vendored board files were written against this line.
- **Licence:** MIT, matching the upstream code being lifted. Every lifted block keeps an attribution comment naming the source file and line range.
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
  refs/
    fetch.sh                      clones the reference repos (gitignored contents)
  boards/teenageengineering/stem_player/
                                  vendored verbatim from sp1-tape-looper (MIT)
  firmware/
    CMakeLists.txt
    prj.conf
    app.overlay                   ADC channels for the 4 faders + battery
    src/
      main.c                      init + the control loop, nothing else
      board_io.c / board_io.h     ADC ladders, button decode, LEDs, WDT, power
      midi_tx.c  / midi_tx.h      bit-banged TRS MIDI TX (lifted)
      profile.c  / profile.h      the config table + the shipped default profile
      controls.c / controls.h     PURE: fader conditioning, button FSM
      buttons.c  / buttons.h      PURE: the unified button behaviour model
      presets.c  / presets.h      PURE: preset record encode/decode/page scan
      presets_flash.c             Zephyr flash IO for the storage page
  tests/host/
    Makefile                      builds and runs every pure-logic test with clang
    test_util.h                   tiny assert runner
    test_controls.c
    test_buttons.c
    test_presets.c
    test_profile.c
```

`controls.c`, `buttons.c`, `presets.c` and `profile.c` must not include any Zephyr header. That is what makes the host tests possible, and it is the single most important structural rule in this plan.

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
- Produces: `refs/fetch.sh` placing `refs/sp1-tape-looper` and `refs/sp1-midi` on disk. Every later task that says "lift from main.c:NNNN" reads from `refs/sp1-tape-looper/firmware/src/main.c`.

- [ ] **Step 1: Write `LICENSE`**

MIT, copyright "2026 Morten Wagner". Add a second paragraph below the licence text:

```
Portions of firmware/src (board bring-up, ADC ladder reads, LED soft-PWM,
watchdog and power handling, and the bit-banged TRS MIDI transmitter) are
derived from chattock/sp1-tape-looper, MIT licensed, and from work by
timknapen (SP-1-dev pin map) and ericlewis (sp1-midi board reference).
The board definition under boards/ is vendored from sp1-tape-looper.
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
# gitignored: they are read-only references for lifted code and line numbers.
set -euo pipefail
cd "$(dirname "$0")"
clone() {
  local url=$1 dir=$2
  if [ -d "$dir/.git" ]; then git -C "$dir" pull --ff-only; else git clone "$url" "$dir"; fi
}
clone https://github.com/chattock/sp1-tape-looper.git sp1-tape-looper
clone https://github.com/ericlewis/sp1-midi.git       sp1-midi
clone https://github.com/timknapen/SP-1-dev.git       SP-1-dev
echo "references ready in $(pwd)"
```

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

**Files:**
- Create: `docs/toolchain.md`

**Interfaces:**
- Consumes: `refs/sp1-tape-looper`.
- Produces: a working `west` in `~/zephyrproject`, and `refs/sp1-tape-looper/build/zephyr/zephyr.bin`, which is the image Phase 1 flashes.

- [ ] **Step 1: Install host dependencies**

```bash
brew install cmake ninja gperf python3 ccache dtc libmagic wget
cmake --version && ninja --version && dtc --version
```
Expected: CMake 3.20 or newer.

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
SRC_buttons  := ../../firmware/src/buttons.c ../../firmware/src/controls.c
SRC_presets  := ../../firmware/src/presets.c
SRC_profile  := ../../firmware/src/profile.c

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

### Task 1.1: Flash and recovery drill on the dev puck

**Files:**
- Create: `docs/flashing.md`

**Interfaces:**
- Produces: a dev puck running the looper firmware, and a written recovery procedure that has actually been executed twice.

- [ ] **Step 1: Mark the dev puck**

Physically label one puck (tape, marker, anything). Record any serial or distinguishing mark in `docs/flashing.md`. The other pucks do not get connected during this project.

- [ ] **Step 2: Serve the local flasher**

```bash
cd "/Users/morten/Documents/Other Creations/dev/solderless/solderless-2026-05-18"
python3 -m http.server 8788
```
Open `http://127.0.0.1:8788/` in Chrome (WebSerial does not exist in Safari).

- [ ] **Step 3: Enter bootloader mode and confirm the device appears**

Power the puck off. Hold Track 1 + Track 4. Plug in USB-C. Release once the Track 1 LED lights. In the launcher, open **device info** and connect. Expected: the page reports device state rather than a connection error. Note what it prints in `docs/flashing.md`.

- [ ] **Step 4: Flash the looper build from Task 0.2**

Open **firmware utility**, select `refs/sp1-tape-looper/build/zephyr/zephyr.bin`, connect, flash. Expected: progress runs to 100 percent and the puck reboots into the looper (LED behaviour per the looper README).

- [ ] **Step 5: Recover, twice**

Re-enter bootloader mode from the running looper firmware (power off, Track 1 + Track 4, plug in). Flash the shipped `refs/sp1-tape-looper/sp1_looper.bin` this time. Then do it once more with your own build. Two successful re-entries with a custom image already resident is the actual thing being tested: that a running app cannot lock you out.

**STOP RULE:** if bootloader mode cannot be re-entered from a running custom firmware, stop the project here and report. Do not write firmware for a device you cannot recover.

- [ ] **Step 6: Write `docs/flashing.md`**

The literal steps that worked, the exact button hold, what the LEDs did at each stage, what the flasher printed, and how long it took. Add a "if it will not enter bootloader" section with whatever you learned.

- [ ] **Step 7: Commit**

```bash
git add docs/flashing.md
git commit -m "docs: flash and recovery drill executed on the dev puck"
```

---

### Task 1.2: Sync jack electrical survey and adapter

**Files:**
- Create: `docs/hardware-notes.md`

**Interfaces:**
- Produces: a confirmed contact map for the SP-1 sync jack and a physical TRS adapter, both documented. Phase 3 depends on the adapter existing.

- [ ] **Step 1: Confirm which jack is which**

The SP-1 has two 3.5 mm jacks. With the looper running and nothing playing, identify the sync jack (the non-audio one). Note how you told them apart.

- [ ] **Step 2: Measure the idle state**

With a multimeter, referenced to USB-C shell ground: measure sleeve (expect continuity to ground, near 0 ohm), ring (expect a steady DC level, hypothesis 3.3 V, this is MIDI idle/mark), and tip (expect near 0 V while stopped, this is the PO sync line). Record the actual numbers.

Interpretation: a steady high on the ring at idle confirms the PNP stage described at `main.c:4405-4410` and that `MIDI_INVERT 1` is correct. A steady low at idle means the polarity is inverted, and Phase 3 will need `MIDI_INVERT 0`. Either way, record it.

- [ ] **Step 3: Watch the ring while the looper transmits**

Start the looper's transport so it emits clock. On a scope, or by watching the DC average fall on the meter, confirm the ring is being modulated. If the ring never moves, the MIDI TX is either compiled out or on a different pin, and Task 1.3 will fail; note it now.

- [ ] **Step 4: Build the adapter**

Working hypothesis, to be tested in Task 1.3: **SP-1 ring to Tiliqua ring, SP-1 sleeve to Tiliqua tip, SP-1 tip unconnected.** Rationale: the Tiliqua input is an opto (`H11L1SR2M`) with 220R in series across the tip and ring of a TRS-A jack, so it needs a current loop, not a logic level. The SP-1 supplies the source on its ring and ground on its sleeve.

Build it from two 3.5 mm TRS pigtails joined with the mapping above (or a breakout board plus jumpers). No series resistor: at 3.3 V through the receiver's 220R the LED sees roughly 8 to 9 mA, which is correct for MIDI and well inside the part's rating.

- [ ] **Step 5: Ask the Discord if the schematic exists**

The sync jack is the only part of this hardware with no published documentation: it is absent from `SP-1-dev/src/stemplayer_pins.h` and its wiki page is a Todo. The looper's constants came from somewhere, and the TE SP-1 DEV Discord (linked from the SP-1-dev README) is the likely home of TimK's sync-jack schematic. One question there could replace an afternoon of probing. Ask, then continue regardless: measurement does not depend on an answer.

- [ ] **Step 6: Write `docs/hardware-notes.md`**

The jack identification, every measured voltage, the adapter wiring diagram in ASCII, and the reasoning above. Note explicitly which facts are measured and which are inherited from the looper's comments, since this is the one area where the community documentation runs out. This file is the one place hardware facts live; later phases cite it instead of re-measuring.

If the schematic does turn up, add it here and say so: it would also be the single most useful thing this project could contribute back to SP-1-dev.

- [ ] **Step 7: Commit**

```bash
git add docs/hardware-notes.md
git commit -m "docs: SP-1 sync jack survey and TRS adapter wiring"
```

---

### Task 1.3: v0a, prove real MIDI bytes leave the puck

**Files:**
- Modify: `docs/hardware-notes.md`

- [ ] **Step 1: Connect to a known-good MIDI input**

SP-1 sync jack, through the Task 1.2 adapter, into any MIDI interface that reaches the Mac (a USB MIDI interface with TRS-A or DIN in; use a TRS-to-DIN adapter if that is what is at hand).

- [ ] **Step 2: Watch raw bytes**

```bash
brew install receivemidi
receivemidi list
receivemidi dev "<your interface name>" clock start stop
```
Alternatively use MIDI Monitor.app. Start the looper's transport.

Expected: a steady stream of clock messages at 24 per quarter note, plus start on transport start and stop on stop.

- [ ] **Step 3: If nothing arrives, work the decision tree in order**

1. Ring not modulating at all (from Task 1.2 Step 3): the TX is not running. Check `MIDI_SYNC_ENABLE` is 1 in the build (`main.c:4436`) and that the looper transport is actually running, since clock only flows when the engine or a tapped grid is active.
2. Bytes arrive but are garbage: polarity. Rebuild the looper with `MIDI_INVERT 0` (`main.c:4422`), reflash, retest. This is why Phase 0 came first.
3. Bytes arrive but the rate is wrong: baud. `MIDI_BIT_US` is 32 for 31250 (`main.c:4423`).
4. Nothing at all and the ring does modulate: the adapter mapping is wrong. Try SP-1 ring to Tiliqua tip with sleeve to ring. If that also fails, the loop has no return path and the adapter needs a bench supply, which is a finding worth stopping on.

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

First custom code. The deliverable is a firmware that boots, shows life, powers down cleanly, and can be recovered. No MIDI yet: nothing is worth debugging on top of an uncertain bring-up.

### Task 2.1: Buildable skeleton with watchdog and clean power-off

**Files:**
- Create: `boards/teenageengineering/stem_player/` (vendored, 6 files)
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
  - `int  board_io_read_fader(int idx);` (0 to 4095 raw, negative on ADC error)
  - `int  board_io_read_track_ladder(void);` (raw code, negative on error)
  - `void board_io_led_set(int idx, uint8_t level);` (centre row, level 0 to 255)
  - `void board_io_track_led_set(int idx, bool on);`

- [ ] **Step 1: Vendor the board definition**

```bash
mkdir -p boards/teenageengineering
cp -R refs/sp1-tape-looper/boards/teenageengineering/stem_player boards/teenageengineering/
ls boards/teenageengineering/stem_player
```
Expected six files: `board.cmake`, `board.yml`, `Kconfig.defconfig`, `Kconfig.stem_player`, `stem_player-pinctrl.dtsi`, `stem_player.dts`, `stem_player_defconfig`.

Then edit `stem_player.dts`: delete the whole `uac2_speaker` node and the `#include <dt-bindings/usb/audio.h>` line, since this firmware has no USB audio. Leave `cdc_acm_uart0` and the `chosen zephyr,console` alone: the serial console is the debug lifeline. Add a comment at the top of the file naming the source repo and licence.

- [ ] **Step 2: Write `firmware/app.overlay`**

Copy the ADC channel block from `refs/sp1-tape-looper/firmware/app.overlay` verbatim (channels 2 to 6, the four faders plus battery) and the `zephyr_user` `io-channels` list. Keep the comment explaining that channels 0 and 1 (the button ladders) come from the board file. Do not change gain, reference or acquisition time: the decode thresholds in Task 2.2 are calibrated to exactly this configuration.

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

target_sources(app PRIVATE
  src/main.c
  src/board_io.c
  src/controls.c
  src/buttons.c
  src/profile.c
  src/presets.c
  src/presets_flash.c
  src/midi_tx.c
)
```

Create empty stubs for the files that later phases fill in, so the build is green from here: each stub is the header include plus nothing. Add them as they are introduced if you prefer, but keep this list as the target.

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

bool board_io_function_held(void);
void board_io_power_off(void);

void board_io_led_set(int idx, uint8_t level);
void board_io_track_led_set(int idx, bool on);

#endif /* SP1_BOARD_IO_H */
```

- [ ] **Step 6: Write `firmware/src/board_io.c` by lifting from the looper**

Lift these blocks from `refs/sp1-tape-looper/firmware/src/main.c`, keeping an attribution comment with the line range on each:

| What | Source lines | Notes |
|---|---|---|
| `struct led`, `leds[]`, `track_leds[]` | 100-110 | pin maps, verified on hardware |
| power/function button pins | 121-123 | P0.27, active low with pull-up |
| BQ24232 charger pins | 126-129 | needed by power-off |
| `BTN_COM` rail | 145-146 | P1.10 must be high before sampling |
| `adc_ladder[]` and the `LAD_*` indices | 143-156 | keep the index meanings identical |
| `ladder_read()` | 192-208 | 2x oversample, returns -1 on error |
| `controls_init()` | 211 onward | raise `BTN_COM`, set up ADC channels |
| LED soft-PWM (`led_pwm_init`, ISR, `led_on`, `led_off`, `track_led_on`, `track_led_off`, `all off`) | 5121-5320 | keep the zero-latency IRQ |
| `feed_wdt()` and watchdog install | 5524-5583 | 4 s window |
| wake-on-button arming | 5585 onward | required by `SYSTEM_OFF` |
| `power_off()` | 5664-5720 | clears both LED rows, clears `RESETREAS`, powers down the external chips, then `SYSTEM_OFF` |
| `g_resetreas` capture at boot | 85 and its boot-time read | BIG FIVE requirement |

Drop everything to do with audio, eMMC, I2S, I2C codecs and the looper engine. `board_io_led_set` takes a 0 to 255 level: map it onto whatever the lifted soft-PWM exposes, and if the lifted renderer is on/off only, threshold at 128 for now and note it as a Phase 6 item.

One thing that is easy to drop by accident: **charging**. The BQ24232's charge-enable pin (`BQ_NCE_PIN`, P0.21) is active low, so `board_io_init` must drive it low or the puck will never charge over USB-C, which the spec lists as expected behaviour. The status pins (`BQ_NCHG`, `BQ_NPGOOD`) are only needed if you want a charge indicator; that is optional in v1. Verify charging works before leaving this task: plug in USB-C with the firmware running and confirm the battery gains charge over a few minutes.

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

Bootloader mode, flash `build/zephyr/zephyr.bin` with the firmware utility. Expected: LED 0 blinks at 1 Hz; holding the function button for 2.5 s powers the device off with all LEDs dark; the puck powers back on normally; bootloader mode still works.

**STOP RULE:** if the puck boots but cannot re-enter bootloader mode, this is the BIG FIVE failing. Recover via the drill from Task 1.1 and do not proceed until it works.

- [ ] **Step 10: Commit**

```bash
git add boards firmware
git commit -m "feat: bootable skeleton with watchdog, LEDs and clean power-off"
```

---

### Task 2.2: Button and fader reads over the console

**Files:**
- Modify: `firmware/src/main.c`
- Modify: `firmware/src/board_io.c`
- Modify: `firmware/src/board_io.h`

**Interfaces:**
- Produces: `int board_io_decode_track_button(int raw);` returning -1 for none, 0 to 3 for the track buttons, 4 for play. Phase 5 consumes it.

- [ ] **Step 1: Add the decode function to `board_io.c`, lifted from main.c:5098-5107**

```c
/* Lifted from sp1-tape-looper firmware/src/main.c:5098-5107 (MIT).
 * Thresholds are calibrated to the exact ADC configuration in the board
 * files and app.overlay: gain 1/6, 0.6 V internal reference, 20 us
 * acquisition, 12-bit. Do not change one without re-measuring the other. */
int board_io_decode_track_button(int v)
{
    if (v <  110) return -1;   /* none          */
    if (v <  300) return 0;    /* track 1, ~213  */
    if (v <  560) return 1;    /* track 2, ~403  */
    if (v <  950) return 2;    /* track 3, ~733  */
    if (v < 1500) return 3;    /* track 4, ~1220 */
    return 4;                  /* play,    ~1823 */
}
```

- [ ] **Step 2: Add a diagnostic loop to `main.c`**

Replace the heartbeat block with this, keeping it behind `SP1_DIAG` so the release build in Phase 8 compiles it out:

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

- [ ] **Step 3: Build, flash, and read the console**

```bash
west build -p -b stem_player firmware -- -DBOARD_ROOT=$(pwd)
# flash, then:
ls /dev/tty.usbmodem*
screen /dev/tty.usbmodem<id> 115200
```

- [ ] **Step 4: Verify against expectations, and record**

Sweep each fader end to end. Expected per fader: a minimum near 0 and a maximum near 3700, monotonic, jitter of only a few counts when untouched. Press each track button and play. Expected: the decoded index matches the physical button, and releasing returns -1.

Write the observed minimum and maximum for each fader into `docs/hardware-notes.md`. If any fader's full-scale differs from 3700 by more than about 5 percent, change `FADER_RAW_FULL` in Task 4.1 to the measured value and say so in the notes.

This step also answers the spec's open risk about whether the faders are absolute or relative: they are absolute analog positions on the SAADC. If a fader instead reads as a jumpy relative sensor, stop and re-plan Phase 4 as pickup mode.

- [ ] **Step 5: Commit**

```bash
git add firmware/src docs/hardware-notes.md
git commit -m "feat: fader and button ladder reads, verified over the console"
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

Set levels 32, 128 and 255 on centre LED 0 for two seconds each. Expected: visibly different brightness. If the lifted renderer is on/off only, note that Phase 6 must render value as a blink rate or a bar across the four centre LEDs instead.

- [ ] **Step 4: Revert the diagnostic, keep the notes, commit**

```bash
git add docs/hardware-notes.md firmware/src/main.c
git commit -m "docs: LED map and brightness capability surveyed on hardware"
```

---

# Phase 3: MIDI transmit

The end-to-end proof. After this task the puck moves a parameter on the synth.

### Task 3.1: Bit-banged TRS MIDI transmitter

**Files:**
- Create: `firmware/src/midi_tx.h`
- Create: `firmware/src/midi_tx.c`
- Modify: `firmware/src/main.c`

**Interfaces:**
- Produces:
  - `void midi_tx_init(void);`
  - `void midi_tx_byte(uint8_t b);`
  - `void midi_tx_cc(uint8_t channel, uint8_t cc, uint8_t value);` (channel is the wire value, 0 to 15)

- [ ] **Step 1: Write `firmware/src/midi_tx.h`**

```c
/* TRS MIDI transmit on the SP-1 sync jack.
 *
 * The sync jack's ring is driven by P0.23 (BC807 base) through a PNP that
 * INVERTS the line, so the waveform is bit-banged rather than handed to a
 * UART peripheral. A hardware timer clocks one bit per ISR with interrupts
 * left on. Lifted from sp1-tape-looper firmware/src/main.c:4398-4530 (MIT).
 */
#ifndef SP1_MIDI_TX_H
#define SP1_MIDI_TX_H

#include <stdint.h>

void midi_tx_init(void);
void midi_tx_byte(uint8_t b);
void midi_tx_cc(uint8_t channel, uint8_t cc, uint8_t value);

#endif /* SP1_MIDI_TX_H */
```

- [ ] **Step 2: Write `firmware/src/midi_tx.c`**

Lift `midi_pins_init`, `midi_line`, `midi_timer_isr`, `midi_timer_init` and `midi_send` from `main.c:4419-4530` unchanged, keeping the constants:

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
void midi_tx_cc(uint8_t channel, uint8_t cc, uint8_t value)
{
    midi_tx_byte((uint8_t)(0xB0u | (channel & 0x0Fu)));
    midi_tx_byte(cc & 0x7Fu);
    midi_tx_byte(value & 0x7Fu);
}
```

`midi_tx_byte` is the lifted `midi_send`, renamed. Note in a comment that a three-byte CC occupies roughly 1 ms on the wire at 31250 baud, which is why the burst spacing in Phase 5 is 1 to 2 ms.

- [ ] **Step 3: Add a MIDI smoke test to `main.c`**

Behind `#define SP1_DIAG 1`, send `midi_tx_cc(0, 102, v)` once every 100 ms with `v` ramping 0 to 127 and back, so cutoff sweeps continuously without touching anything.

- [ ] **Step 4: Build and flash**

```bash
west build -p -b stem_player firmware -- -DBOARD_ROOT=$(pwd)
```

- [ ] **Step 5: Verify on the MIDI monitor first**

Same rig as Task 1.3. Expected: a stream of `channel 1 control-change 102` messages with a ramping value, roughly 10 per second, no framing errors. Debug here, not at the rack: this is a two-cable bench setup.

- [ ] **Step 6: Verify on the Tiliqua**

SP-1 into the Tiliqua MIDI-in with popgoblin running. Expected: the on-screen MIDI activity indicator lights (`gateware/src/rs/lib/src/ui.rs:57`, shown while activity is under 100 ms old) and the cutoff option value sweeps on the display. This is the end-to-end proof the spec's v0 was after.

- [ ] **Step 7: Record and commit**

Append the result to `docs/hardware-notes.md`, including the final `MIDI_INVERT` value.

```bash
git add firmware/src/midi_tx.c firmware/src/midi_tx.h firmware/src/main.c docs/hardware-notes.md
git commit -m "feat: bit-banged TRS MIDI TX, cutoff sweep confirmed on the synth"
```

---

# Phase 4: Faders to CC

### Task 4.1: Fader conditioning, pure logic

**Files:**
- Create: `firmware/src/controls.h`
- Create: `firmware/src/controls.c`
- Create: `tests/host/test_controls.c`
- Modify: `tests/host/Makefile` (add `controls` to `TESTS`)

**Interfaces:**
- Produces:
  - `uint8_t fader_raw_to_cc(int raw);`
  - `bool fader_update(fader_state_t *st, int raw, uint32_t now_ms, uint8_t *out_value);`

- [ ] **Step 1: Write the failing test, `tests/host/test_controls.c`**

```c
#include "controls.h"
#include "test_util.h"

static void test_raw_to_cc_endpoints(void)
{
    CHECK_EQ(fader_raw_to_cc(0), 0);
    CHECK_EQ(fader_raw_to_cc(FADER_RAW_FULL), 127);
    CHECK_EQ(fader_raw_to_cc(FADER_RAW_FULL + 500), 127);  /* clamped */
    CHECK_EQ(fader_raw_to_cc(-1), 0);                      /* defensive */
}

static void test_raw_to_cc_midpoint(void)
{
    /* 1850 of 3700 is exactly half scale; rounded, that is 64. */
    CHECK_EQ(fader_raw_to_cc(1850), 64);
}

static void test_first_update_always_emits(void)
{
    fader_state_t st = {0};
    uint8_t v = 0xFF;
    CHECK(fader_update(&st, 0, 1000, &v));
    CHECK_EQ(v, 0);
}

static void test_adc_error_never_emits(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, -1, 1000, &v));
}

static void test_jitter_inside_deadband_is_ignored(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(fader_update(&st, 1000, 0, &v));            /* first emission */
    CHECK(!fader_update(&st, 1004, 100, &v));         /* +4 counts */
    CHECK(!fader_update(&st, 996,  200, &v));         /* -4 counts */
}

static void test_real_move_emits_after_deadband(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(fader_update(&st, 1000, 0, &v));
    uint8_t first = v;
    CHECK(fader_update(&st, 1100, 100, &v));
    CHECK(v != first);
}

static void test_rate_limit_holds_then_releases(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(fader_update(&st, 1000, 0, &v));
    /* A large move 3 ms later is suppressed by the 10 ms per-fader limit. */
    CHECK(!fader_update(&st, 1400, 3, &v));
    /* The same position at 12 ms goes out: the move is retried, not lost. */
    CHECK(fader_update(&st, 1400, 12, &v));
    CHECK_EQ(v, fader_raw_to_cc(1400));
}

static void test_same_cc_value_is_not_resent(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    /* A 7-bit step spans about 29 raw counts, and CC 34 covers raw 976 to
     * 1005. Moving from 980 to 1000 clears the deadband but stays inside
     * that one bucket, so nothing goes out. Pick the pair from the bucket
     * arithmetic, not by eye: 1000 to 1020 crosses into CC 35. */
    CHECK(fader_update(&st, 980, 0, &v));
    CHECK_EQ(v, 34);
    CHECK(!fader_update(&st, 1000, 500, &v));
}

int main(void)
{
    RUN(test_raw_to_cc_endpoints);
    RUN(test_raw_to_cc_midpoint);
    RUN(test_first_update_always_emits);
    RUN(test_adc_error_never_emits);
    RUN(test_jitter_inside_deadband_is_ignored);
    RUN(test_real_move_emits_after_deadband);
    RUN(test_rate_limit_holds_then_releases);
    RUN(test_same_cc_value_is_not_resent);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
sed -i '' 's/^TESTS := smoke/TESTS := smoke controls/' tests/host/Makefile
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

/* Full-scale raw code of a fader on the 12-bit SAADC as configured in
 * app.overlay. Measured on hardware in Task 2.2; the looper uses the same
 * number (main.c:7487). */
#define FADER_RAW_FULL          3700

/* Raw counts a fader must move past its last emitted position before a new
 * value is considered. One 7-bit step is about 29 raw counts, so 8 rejects
 * ADC jitter without adding perceptible lag. */
#define FADER_DEADBAND_RAW      8

/* Minimum spacing between messages from a single fader. */
#define FADER_MIN_INTERVAL_MS   10

typedef struct {
    int      last_raw;
    uint8_t  last_sent;
    bool     have_sent;
    uint32_t last_send_ms;
} fader_state_t;

/* Scale a raw SAADC code to a 7-bit CC value, rounded and clamped. */
uint8_t fader_raw_to_cc(int raw);

/* Feed one reading. Returns true when a CC should be transmitted, and then
 * writes the value to out_value. A negative raw (ADC error) never emits.
 * A move suppressed by the rate limit is retried on the next call rather
 * than dropped, so the final resting position is always sent. */
bool fader_update(fader_state_t *st, int raw, uint32_t now_ms, uint8_t *out_value);

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

bool fader_update(fader_state_t *st, int raw, uint32_t now_ms, uint8_t *out_value)
{
    if (raw < 0) {
        return false;               /* ADC error: hold the last value */
    }

    if (!st->have_sent) {
        st->last_raw      = raw;
        st->last_sent     = fader_raw_to_cc(raw);
        st->have_sent     = true;
        st->last_send_ms  = now_ms;
        *out_value        = st->last_sent;
        return true;
    }

    if (abs_diff(raw, st->last_raw) < FADER_DEADBAND_RAW) {
        return false;
    }

    uint8_t v = fader_raw_to_cc(raw);
    if (v == st->last_sent) {
        return false;
    }

    if ((uint32_t)(now_ms - st->last_send_ms) < FADER_MIN_INTERVAL_MS) {
        return false;               /* retried on the next pass */
    }

    st->last_raw     = raw;
    st->last_sent    = v;
    st->last_send_ms = now_ms;
    *out_value       = v;
    return true;
}
```

- [ ] **Step 5: Run the tests**

```bash
make -C tests/host test
```
Expected: PASS for both `smoke` and `controls`. Every test in this task and in Tasks 4.2, 5.1, 5.2 and 7.1 was compiled and run against exactly this implementation while the plan was written, with `-Wall -Wextra -Werror`, so a failure here means a transcription slip rather than a design problem.

A note on the deadband, since the two constants interact: `FADER_DEADBAND_RAW` (8) is deliberately smaller than one 7-bit step (about 29 raw counts). It is not what stops repeat sends; the `v == st->last_sent` check is. The deadband exists to stop the arithmetic running at all on noise, and it is anchored to the last *emitted* raw position rather than the last reading, so a fader parked exactly on a bucket boundary can only oscillate if its jitter exceeds 8 counts. The looper measured that jitter at plus or minus 1 (`main.c:2685`), leaving 8x of margin. If Task 2.2 measures worse on this unit, raise the constant and re-run these tests.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/controls.c firmware/src/controls.h tests/host
git commit -m "feat: fader conditioning with deadband and per-fader rate limit"
```

---

### Task 4.2: The profile table

**Files:**
- Create: `firmware/src/profile.h`
- Create: `firmware/src/profile.c`
- Create: `tests/host/test_profile.c`
- Modify: `tests/host/Makefile`

**Interfaces:**
- Produces: `profile_t`, `fader_cfg_t`, `button_cfg_t`, `btn_mode_t`, and `extern const profile_t profile_popgoblin_default;`. Phases 5, 6 and 7 all read this table and never hard-code a CC number.

- [ ] **Step 1: Write the failing test, `tests/host/test_profile.c`**

```c
#include "profile.h"
#include "test_util.h"

static void test_default_profile_matches_the_spec(void)
{
    const profile_t *p = &profile_popgoblin_default;

    CHECK_EQ(p->fader[0].cc, 102);   /* filter cutoff  */
    CHECK_EQ(p->fader[1].cc, 104);   /* reverb wet     */
    CHECK_EQ(p->fader[2].cc, 107);   /* delay time     */
    CHECK_EQ(p->fader[3].cc, 108);   /* delay feedback */

    for (int i = 0; i < PROFILE_NUM_FADERS; i++) {
        CHECK_EQ(p->fader[i].channel, 0);   /* MIDI channel 1 on the wire */
    }

    CHECK_EQ(p->button[0].mode, BTN_MODE_LATCH_MOMENTARY);
    CHECK_EQ(p->button[0].cc,   64);
    CHECK_EQ(p->button[1].mode, BTN_MODE_STEP);
    CHECK_EQ(p->button[1].cc,   105);
    CHECK_EQ(p->button[1].n_steps, 4);
    CHECK_EQ(p->button[2].mode, BTN_MODE_PRESET);
    CHECK_EQ(p->button[2].preset_slot, 0);
    CHECK_EQ(p->button[3].mode, BTN_MODE_PRESET);
    CHECK_EQ(p->button[3].preset_slot, 1);
}

static void test_step_values_span_the_range(void)
{
    const button_cfg_t *b = &profile_popgoblin_default.button[1];
    CHECK_EQ(b->steps[0], 0);
    CHECK_EQ(b->steps[3], 127);
    for (int i = 1; i < b->n_steps; i++) {
        CHECK(b->steps[i] > b->steps[i - 1]);
    }
}

static void test_preset_capture_list_uses_existing_ccs_only(void)
{
    const profile_t *p = &profile_popgoblin_default;
    CHECK_EQ(p->preset_capture_len, 5);
    /* Every captured CC must be one the surface already drives, and CC 64
     * (freeze) must never be captured: it is a live gesture, not a scene. */
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
    RUN(test_step_values_span_the_range);
    RUN(test_preset_capture_list_uses_existing_ccs_only);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
sed -i '' 's/^TESTS := smoke controls/TESTS := smoke controls profile/' tests/host/Makefile
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
 * this table at runtime is parked for v1.1. */
#ifndef SP1_PROFILE_H
#define SP1_PROFILE_H

#include <stdint.h>

#define PROFILE_NUM_FADERS   4
#define PROFILE_NUM_BUTTONS  4
#define PROFILE_MAX_STEPS    4
/* Kept in step with PRESET_ENTRIES in presets.h: a scene that does not fit
 * a stored record would be silently truncated. */
#define PROFILE_MAX_CAPTURE  5

typedef enum {
    /* Tap latches the CC on, tap again latches it off; a hold behaves as a
     * momentary pedal that releases when the finger lifts. */
    BTN_MODE_LATCH_MOMENTARY = 0,
    /* Each tap sends the next value in steps[]. */
    BTN_MODE_STEP = 1,
    /* Tap replays a stored list of (cc, value) pairs; a long hold stores
     * the current surface state into that slot. */
    BTN_MODE_PRESET = 2,
} btn_mode_t;

typedef struct {
    uint8_t cc;
    uint8_t channel;        /* wire value, 0 to 15 */
} fader_cfg_t;

typedef struct {
    btn_mode_t mode;
    uint8_t    channel;
    uint8_t    cc;                         /* LATCH_MOMENTARY and STEP */
    uint8_t    on_value;                   /* LATCH_MOMENTARY: the "on" value */
    uint8_t    off_value;                  /* LATCH_MOMENTARY: the "off" value */
    uint8_t    steps[PROFILE_MAX_STEPS];   /* STEP */
    uint8_t    n_steps;                    /* STEP */
    uint8_t    preset_slot;                /* PRESET */
} button_cfg_t;

typedef struct {
    fader_cfg_t  fader[PROFILE_NUM_FADERS];
    button_cfg_t button[PROFILE_NUM_BUTTONS];
    /* The CCs a preset snapshot captures and replays, in send order. */
    uint8_t      preset_capture[PROFILE_MAX_CAPTURE];
    uint8_t      preset_capture_len;
} profile_t;

extern const profile_t profile_popgoblin_default;

#endif /* SP1_PROFILE_H */
```

- [ ] **Step 4: Write `firmware/src/profile.c`**

```c
#include "profile.h"

/* PopGoblin CC map, read from the synth's own firmware
 * (tiliqua gateware/src/top/popgoblin/fw/src/main.rs:60-70):
 *   102 cutoff, 103 reso, 104 reverb wet, 105 shimmer, 106 chorus rate,
 *   107 delay time, 108 delay feedback, 64 freeze, 43 palette, 51 plot.
 * The synth ignores the channel (it parses ControlChange(_, cc, val)), so
 * channel 0 here is a convention, not a requirement. */
const profile_t profile_popgoblin_default = {
    .fader = {
        { .cc = 102, .channel = 0 },   /* filter cutoff        */
        { .cc = 104, .channel = 0 },   /* reverb wet           */
        { .cc = 107, .channel = 0 },   /* delay time           */
        { .cc = 108, .channel = 0 },   /* delay feedback       */
    },
    .button = {
        {   /* freeze: the synth holds while the last value seen is >= 64 */
            .mode = BTN_MODE_LATCH_MOMENTARY, .channel = 0, .cc = 64,
            .on_value = 127, .off_value = 0,
        },
        {   /* shimmer: a 4-step option, so send one value per quarter of
             * the 7-bit range and let the synth bucket it */
            .mode = BTN_MODE_STEP, .channel = 0, .cc = 105,
            .steps = { 0, 42, 85, 127 }, .n_steps = 4,
        },
        { .mode = BTN_MODE_PRESET, .channel = 0, .preset_slot = 0 },
        { .mode = BTN_MODE_PRESET, .channel = 0, .preset_slot = 1 },
    },
    /* Freeze (64) is deliberately absent: a scene should not re-trigger a
     * live gesture. */
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

### Task 4.3: Wire the faders to the wire

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
        uint32_t now = (uint32_t)k_uptime_get();

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
            if (fader_update(&g_fader[i], board_io_read_fader(i), now, &v)) {
                midi_tx_cc(prof->fader[i].channel, prof->fader[i].cc, v);
            }
        }

        k_msleep(CONTROL_PERIOD_MS);
    }
    return 0;
}
```

Note on timing: all four faders are read every pass. The looper round-robins them because its ADC reads compete with an eMMC streamer; this firmware has no such competition, so four blocking reads per 5 ms pass are affordable. If the console shows the loop overrunning, fall back to round-robin and raise `FADER_MIN_INTERVAL_MS` to match.

- [ ] **Step 2: Build, flash, and test at the bench with the MIDI monitor**

Expected: moving fader 1 produces CC 102 messages, fader 2 gives 104, fader 3 gives 107, fader 4 gives 108. An untouched fader produces nothing at all (this is the zipper test: any idle chatter means the deadband is too small for this unit, so raise `FADER_DEADBAND_RAW` and re-run the host tests).

- [ ] **Step 3: Test at the rack**

Expected: each fader moves its parameter on the popgoblin display, sweeps are smooth, and releasing a fader leaves the parameter exactly where the fader is (no lost final value, which is what the rate-limit retry protects).

- [ ] **Step 4: Commit**

```bash
git add firmware/src/main.c
git commit -m "feat: four faders drive their profile CCs on the synth"
```

---

# Phase 5: Buttons

### Task 5.1: Button edge and hold detection, pure logic

**Files:**
- Modify: `firmware/src/controls.h`
- Modify: `firmware/src/controls.c`
- Modify: `tests/host/test_controls.c`

**Interfaces:**
- Produces:
  - `uint8_t btn_update(btn_state_t *st, bool pressed_now, uint32_t now_ms);` returning a bitmask of `BTN_EV_PRESS`, `BTN_EV_HOLD`, `BTN_EV_TAP`, `BTN_EV_RELEASE`.
  - `uint32_t btn_held_ms(const btn_state_t *st, uint32_t now_ms);`

- [ ] **Step 1: Add the failing tests to `tests/host/test_controls.c`**

```c
static void test_press_then_short_release_is_a_tap(void)
{
    btn_state_t st = {0};
    CHECK_EQ(btn_update(&st, true,  0),   BTN_EV_PRESS);
    CHECK_EQ(btn_update(&st, true,  50),  0);
    CHECK_EQ(btn_update(&st, false, 120), BTN_EV_RELEASE | BTN_EV_TAP);
}

static void test_hold_fires_once_and_release_is_not_a_tap(void)
{
    btn_state_t st = {0};
    CHECK_EQ(btn_update(&st, true, 0), BTN_EV_PRESS);
    CHECK_EQ(btn_update(&st, true, BTN_HOLD_MS - 1), 0);
    CHECK_EQ(btn_update(&st, true, BTN_HOLD_MS), BTN_EV_HOLD);
    CHECK_EQ(btn_update(&st, true, BTN_HOLD_MS + 500), 0);   /* only once */
    CHECK_EQ(btn_update(&st, false, BTN_HOLD_MS + 600), BTN_EV_RELEASE);
}

static void test_held_ms_measures_from_the_press(void)
{
    btn_state_t st = {0};
    btn_update(&st, true, 1000);
    CHECK_EQ(btn_held_ms(&st, 1350), 350);
}

static void test_idle_button_reports_nothing(void)
{
    btn_state_t st = {0};
    CHECK_EQ(btn_update(&st, false, 0), 0);
    CHECK_EQ(btn_update(&st, false, 5000), 0);
}
```
Add the four `RUN(...)` lines to `main`.

- [ ] **Step 2: Run and watch them fail**

```bash
make -C tests/host test
```
Expected: FAIL, `btn_update` not declared.

- [ ] **Step 3: Extend `firmware/src/controls.h`**

```c
/* Hold threshold for "store this preset". Long enough that a performance
 * tap can never reach it. */
#define BTN_HOLD_MS        2000

/* Above this, a press is treated as a pedal gesture rather than a tap. */
#define BTN_MOMENTARY_MS   300

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
uint8_t  btn_update(btn_state_t *st, bool pressed_now, uint32_t now_ms);
uint32_t btn_held_ms(const btn_state_t *st, uint32_t now_ms);
```

- [ ] **Step 4: Extend `firmware/src/controls.c`**

```c
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

uint32_t btn_held_ms(const btn_state_t *st, uint32_t now_ms)
{
    return st->down ? (uint32_t)(now_ms - st->down_at_ms) : 0u;
}
```

- [ ] **Step 5: Run the tests**

```bash
make -C tests/host test
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/controls.c firmware/src/controls.h tests/host/test_controls.c
git commit -m "feat: button press, hold, tap and release detection"
```

---

### Task 5.2: The unified button behaviour model, pure logic

**Files:**
- Create: `firmware/src/buttons.h`
- Create: `firmware/src/buttons.c`
- Create: `tests/host/test_buttons.c`
- Modify: `tests/host/Makefile`

**Interfaces:**
- Produces:
  - `void button_engine_init(button_engine_t *e, const profile_t *prof);`
  - `int  button_engine_event(button_engine_t *e, int idx, uint8_t ev, uint32_t held_ms, cc_msg_t *out, int out_max);` returning the number of messages to send, or a negative value for "the caller must handle a preset action" (see below).
  - `typedef struct { uint8_t channel, cc, value; } cc_msg_t;`

Everything a button does is "emit a list of (channel, cc, value)". The engine never talks to hardware and never sleeps: the caller sends the list with the 1 to 2 ms spacing.

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

/* --- freeze: tap latches, second tap releases --- */

static void test_freeze_press_sends_on(void)
{
    setup();
    int n = button_engine_event(&eng, 0, BTN_EV_PRESS, 0, out, OUT_MAX);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].cc, 64);
    CHECK_EQ(out[0].value, 127);
}

static void test_freeze_tap_latches_on_release(void)
{
    setup();
    button_engine_event(&eng, 0, BTN_EV_PRESS, 0, out, OUT_MAX);
    /* short press: nothing is sent on release, the freeze stays on */
    int n = button_engine_event(&eng, 0, BTN_EV_RELEASE | BTN_EV_TAP, 120,
                                out, OUT_MAX);
    CHECK_EQ(n, 0);
}

static void test_freeze_second_tap_sends_off(void)
{
    setup();
    button_engine_event(&eng, 0, BTN_EV_PRESS, 0, out, OUT_MAX);
    button_engine_event(&eng, 0, BTN_EV_RELEASE | BTN_EV_TAP, 120, out, OUT_MAX);
    int n = button_engine_event(&eng, 0, BTN_EV_PRESS, 0, out, OUT_MAX);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].cc, 64);
    CHECK_EQ(out[0].value, 0);
}

static void test_freeze_long_press_is_momentary(void)
{
    setup();
    button_engine_event(&eng, 0, BTN_EV_PRESS, 0, out, OUT_MAX);
    /* held past the momentary threshold: release sends off */
    int n = button_engine_event(&eng, 0, BTN_EV_RELEASE, 900, out, OUT_MAX);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].cc, 64);
    CHECK_EQ(out[0].value, 0);
}

/* --- shimmer: each tap steps --- */

static void test_shimmer_steps_and_wraps(void)
{
    setup();
    const uint8_t want[5] = { 42, 85, 127, 0, 42 };
    for (int i = 0; i < 5; i++) {
        int n = button_engine_event(&eng, 1, BTN_EV_RELEASE | BTN_EV_TAP, 100,
                                    out, OUT_MAX);
        CHECK_EQ(n, 1);
        CHECK_EQ(out[0].cc, 105);
        CHECK_EQ(out[0].value, want[i]);
    }
}

static void test_shimmer_press_alone_sends_nothing(void)
{
    setup();
    CHECK_EQ(button_engine_event(&eng, 1, BTN_EV_PRESS, 0, out, OUT_MAX), 0);
}

/* --- presets --- */

static void test_preset_tap_replays_the_stored_list(void)
{
    setup();
    cc_msg_t scene[3] = {
        { 0, 102, 30 }, { 0, 104, 90 }, { 0, 107, 12 },
    };
    button_engine_set_preset(&eng, 0, scene, 3);
    int n = button_engine_event(&eng, 2, BTN_EV_RELEASE | BTN_EV_TAP, 100,
                                out, OUT_MAX);
    CHECK_EQ(n, 3);
    CHECK_EQ(out[0].cc, 102);
    CHECK_EQ(out[0].value, 30);
    CHECK_EQ(out[2].cc, 107);
    CHECK_EQ(out[2].value, 12);
}

static void test_empty_preset_sends_nothing(void)
{
    setup();
    CHECK_EQ(button_engine_event(&eng, 2, BTN_EV_RELEASE | BTN_EV_TAP, 100,
                                 out, OUT_MAX), 0);
}

static void test_preset_hold_requests_a_save(void)
{
    setup();
    int n = button_engine_event(&eng, 3, BTN_EV_HOLD, BTN_HOLD_MS,
                                out, OUT_MAX);
    CHECK_EQ(n, BUTTON_ACTION_SAVE_PRESET);
    CHECK_EQ(button_engine_pending_save_slot(&eng), 1);
}

static void test_release_after_a_save_does_not_also_replay(void)
{
    setup();
    button_engine_event(&eng, 3, BTN_EV_HOLD, BTN_HOLD_MS, out, OUT_MAX);
    CHECK_EQ(button_engine_event(&eng, 3, BTN_EV_RELEASE, BTN_HOLD_MS + 100,
                                 out, OUT_MAX), 0);
}

int main(void)
{
    RUN(test_freeze_press_sends_on);
    RUN(test_freeze_tap_latches_on_release);
    RUN(test_freeze_second_tap_sends_off);
    RUN(test_freeze_long_press_is_momentary);
    RUN(test_shimmer_steps_and_wraps);
    RUN(test_shimmer_press_alone_sends_nothing);
    RUN(test_preset_tap_replays_the_stored_list);
    RUN(test_empty_preset_sends_nothing);
    RUN(test_preset_hold_requests_a_save);
    RUN(test_release_after_a_save_does_not_also_replay);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run and watch it fail**

```bash
sed -i '' 's/^TESTS := smoke controls profile/TESTS := smoke controls profile buttons/' tests/host/Makefile
make -C tests/host test
```
Expected: FAIL, `buttons.h` not found.

- [ ] **Step 3: Write `firmware/src/buttons.h`**

```c
/* PURE. The unified button model: every button is "send a list of
 * (channel, cc, value)". Freeze is a one-item toggling list, shimmer is a
 * one-item cycling list, a preset is a short list replayed on the CCs the
 * faders already drive. No new CCs, and therefore no synth-side work. */
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
    uint8_t channel;
    uint8_t cc;
    uint8_t value;
} cc_msg_t;

typedef struct {
    cc_msg_t msg[PROFILE_MAX_CAPTURE];
    uint8_t  len;
} preset_slot_t;

typedef struct {
    const profile_t *prof;
    bool     latched[PROFILE_NUM_BUTTONS];    /* LATCH_MOMENTARY state */
    bool     pending_pedal[PROFILE_NUM_BUTTONS];
    uint8_t  step_idx[PROFILE_NUM_BUTTONS];   /* STEP position */
    bool     save_armed[PROFILE_NUM_BUTTONS]; /* a hold consumed the release */
    int      pending_save_slot;
    preset_slot_t preset[BUTTON_MAX_PRESET_SLOTS];
} button_engine_t;

void button_engine_init(button_engine_t *e, const profile_t *prof);

/* Feed one button event mask from btn_update. Returns the number of
 * messages written to out, 0 for nothing to send, or
 * BUTTON_ACTION_SAVE_PRESET when the caller must snapshot the surface into
 * button_engine_pending_save_slot(e) and then call button_engine_set_preset. */
int  button_engine_event(button_engine_t *e, int idx, uint8_t ev,
                         uint32_t held_ms, cc_msg_t *out, int out_max);

int  button_engine_pending_save_slot(const button_engine_t *e);
void button_engine_set_preset(button_engine_t *e, int slot,
                              const cc_msg_t *msgs, int len);
const preset_slot_t *button_engine_get_preset(const button_engine_t *e, int slot);

/* For the LED layer: current step index and latch state. */
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
                        uint32_t held_ms, cc_msg_t *out, int out_max)
{
    if (idx < 0 || idx >= PROFILE_NUM_BUTTONS) {
        return 0;
    }
    const button_cfg_t *cfg = &e->prof->button[idx];
    int n = 0;

    switch (cfg->mode) {

    case BTN_MODE_LATCH_MOMENTARY:
        /* Press acts immediately: a performer expects freeze the instant
         * the finger lands. What the RELEASE means depends on how long the
         * press lasted, which is the whole trick. */
        if (ev & BTN_EV_PRESS) {
            if (e->latched[idx]) {
                e->latched[idx]       = false;
                e->pending_pedal[idx] = false;
                return emit(out, out_max, n, cfg->channel, cfg->cc,
                            cfg->off_value);
            }
            e->pending_pedal[idx] = true;
            return emit(out, out_max, n, cfg->channel, cfg->cc, cfg->on_value);
        }
        if (ev & BTN_EV_RELEASE) {
            if (!e->pending_pedal[idx]) {
                return 0;
            }
            e->pending_pedal[idx] = false;
            if (held_ms >= BTN_MOMENTARY_MS) {
                e->latched[idx] = false;     /* it was a pedal */
                return emit(out, out_max, n, cfg->channel, cfg->cc,
                            cfg->off_value);
            }
            e->latched[idx] = true;          /* it was a tap: stay on */
            return 0;
        }
        return 0;

    case BTN_MODE_STEP:
        if (ev & BTN_EV_TAP) {
            if (cfg->n_steps == 0) {
                return 0;
            }
            e->step_idx[idx] = (uint8_t)((e->step_idx[idx] + 1) % cfg->n_steps);
            return emit(out, out_max, n, cfg->channel, cfg->cc,
                        cfg->steps[e->step_idx[idx]]);
        }
        return 0;

    case BTN_MODE_PRESET:
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
Expected: PASS for all four suites.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/buttons.c firmware/src/buttons.h tests/host
git commit -m "feat: unified button model, freeze latch/pedal, step and preset"
```

---

### Task 5.3: Wire the buttons, with debounce and burst pacing

**Files:**
- Modify: `firmware/src/main.c`

- [ ] **Step 1: Add sticky debounce for the shared ladder**

The track buttons sit on one noisy resistor ladder, so a single raw read at a band boundary can name the wrong button. The looper commits a new value only after three consecutive agreeing reads (`main.c:7513-7519`). Do the same:

```c
/* Three agreeing passes (15 ms) before a ladder reading is believed. A
 * finger transiting the ladder cannot fire a neighbouring button. */
static int debounced_track_button(void)
{
    static int committed = -1, candidate = -1, count;
    int raw = board_io_read_track_ladder();
    if (raw < 0) {
        return committed;               /* ADC error: hold */
    }
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

- [ ] **Step 2: Add the send helper with burst spacing**

```c
/* A CC is 3 bytes, about 1 ms on the wire at 31250 baud. Spacing messages
 * by 2 ms keeps a preset burst gentle on the synth's MIDI FIFO. */
static void send_msgs(const cc_msg_t *msgs, int n)
{
    for (int i = 0; i < n; i++) {
        midi_tx_cc(msgs[i].channel, msgs[i].cc, msgs[i].value);
        if (i + 1 < n) {
            k_msleep(2);
        }
    }
}
```

- [ ] **Step 3: Feed the engine from the control loop**

Add `#include "buttons.h"` to `main.c`, declare the engine next to `g_fader`, and initialise it in `main` before the loop:

```c
static button_engine_t     eng;
static btn_state_t         g_btn[PROFILE_NUM_BUTTONS];
```

```c
    button_engine_init(&eng, prof);
```

Then, inside the loop after the fader block:

```c
        int pressed = debounced_track_button();   /* -1, 0..3, or 4 for play */

        for (int i = 0; i < PROFILE_NUM_BUTTONS; i++) {
            uint8_t ev = btn_update(&g_btn[i], pressed == i, now);
            if (!ev) {
                continue;
            }
            cc_msg_t msgs[PROFILE_MAX_CAPTURE];
            int n = button_engine_event(&eng, i, ev, btn_held_ms(&g_btn[i], now),
                                        msgs, (int)(sizeof(msgs) / sizeof(msgs[0])));
            if (n == BUTTON_ACTION_SAVE_PRESET) {
                /* TODO(Phase 7): snapshot the surface and persist it. */
                continue;
            }
            if (n > 0) {
                send_msgs(msgs, n);
            }
        }
```

Note that `send_msgs` sleeps between messages of a burst, which stretches that control pass. With at most five messages that is 8 ms of extra latency on a preset recall only, well inside the 4 s watchdog, and no fader is being swept at the moment a preset button is tapped. If that ever becomes a problem, move sending to a work queue rather than shortening the spacing.

Note: only one ladder button can be read at a time by construction, so simultaneous presses are not supported. That matches the spec's surface, which has no chords.

- [ ] **Step 4: Build, flash, and test at the rack**

Expected, in this order:
1. Tap track 1: freeze engages and stays on. Tap again: it releases.
2. Press and hold track 1 for a second: freeze engages, and releases the moment the finger lifts.
3. Tap track 2 four times: shimmer steps through its four states and returns to the start.
4. Tap track 3 or 4: nothing yet (no preset stored).
5. No stuck freeze, ever. If a release is ever missed, freeze stays on and the session is ruined, so test this one hard, including fast repeated taps.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/main.c
git commit -m "feat: track buttons drive freeze and shimmer on the synth"
```

---

# Phase 6: LEDs as readout

### Task 6.1: Panel feedback

**Files:**
- Modify: `firmware/src/main.c`
- Modify: `firmware/src/board_io.c` (only if Task 2.3 found brightness control is unavailable)

- [ ] **Step 1: Mirror fader values on the centre row**

After the fader loop, render each fader's last-sent value as brightness. The puck then doubles as a readout of the four macro states, which is what the spec asked for, rendered on the LEDs that actually exist.

```c
        for (int i = 0; i < PROFILE_NUM_FADERS; i++) {
            uint8_t level = g_fader[i].have_sent
                          ? (uint8_t)((g_fader[i].last_sent * 255) / 127)
                          : 0u;
            board_io_led_set(i, level);
        }
```

If Task 2.3 established that the LEDs are on/off only, render instead as: LED lit when the value is above 64, plus a short blink on every change. Write down which rendering you implemented and why in `docs/hardware-notes.md`.

- [ ] **Step 2: Mirror button state on the track row**

```c
        board_io_track_led_set(0, button_engine_is_latched(&eng, 0));
        /* Shimmer: the step index shown as one of four brightness levels,
         * so step 0 reads dark and the top step reads full. */
        board_io_track_led_set(1, button_engine_step_index(&eng, 1) != 0);
        for (int b = 2; b < PROFILE_NUM_BUTTONS; b++) {
            const preset_slot_t *p =
                button_engine_get_preset(&eng, prof->button[b].preset_slot);
            board_io_track_led_set(b, p && p->len > 0);
        }
```

The track row is on/off in the lifted renderer, so shimmer shows only "not off" here. If Task 2.3 found that the track LEDs also accept brightness, use `board_io_led_set`-style levels for shimmer instead and say so in the notes.

- [ ] **Step 3: Add the link heartbeat**

The spec asks for a play-button LED heartbeat. There is no separate play LED in the verified map, so use centre LED 3 if Task 2.3 found it unused by the fader readout, otherwise a brief brightness dip on the relevant fader LED at each transmission. Keep it subtle: this is confirmation, not decoration. Record the choice.

- [ ] **Step 4: Verify on hardware**

Expected: moving a fader visibly changes its LED; freeze shows as a lit track LED and clears when released; shimmer brightness steps with each tap. Nothing flickers when the puck is left alone (a flickering idle LED means the fader deadband is letting jitter through and Phase 4's constant needs raising).

- [ ] **Step 5: Commit**

```bash
git add firmware/src docs/hardware-notes.md
git commit -m "feat: LED readout for fader values and button state"
```

---

# Phase 7: Presets

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
sed -i '' 's/^TESTS := smoke controls profile buttons/TESTS := smoke controls profile buttons presets/' tests/host/Makefile
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
 *   [39]     CRC-8 over bytes 0 to 38
 * Five entries per slot matches the default profile's five-CC capture
 * list exactly. If PROFILE_MAX_CAPTURE ever grows past 5, grow
 * PRESET_ENTRIES and PRESET_REC_SIZE together: silently truncating a
 * scene is worse than refusing to build. */
#define PRESET_ENTRIES 5

void preset_record_encode(const preset_bank_t *bank, uint8_t rec[PRESET_REC_SIZE]);
bool preset_record_decode(const uint8_t rec[PRESET_REC_SIZE], preset_bank_t *out);

/* Index of the newest valid record, or -1 when the page holds none. */
int  preset_page_find_latest(const uint8_t *page, uint32_t page_len,
                             preset_bank_t *out);

/* Byte offset of the next writable slot, or -1 when the page is full. */
int  preset_page_next_offset(const uint8_t *page, uint32_t page_len);

#endif /* SP1_PRESETS_H */
```

Add a compile-time guard directly under the defines so the two can never drift:

```c
#if PROFILE_MAX_CAPTURE > PRESET_ENTRIES
#error "preset records cannot hold the profile's capture list"
#endif
```

Both are 5 today, so the guard is silent. It exists so that a future profile with a longer capture list fails the build instead of quietly losing the tail of a scene.

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
Expected: PASS for all five suites.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/presets.c firmware/src/presets.h tests/host
git commit -m "feat: preset record format with CRC and single-page append-log"
```

---

### Task 7.2: Flash storage and the save gesture

**Files:**
- Create: `firmware/src/presets_flash.c`
- Modify: `firmware/src/presets.h` (add the three IO prototypes)
- Modify: `firmware/src/main.c`

**Interfaces:**
- Produces:
  - `bool preset_store_load(preset_bank_t *out);`
  - `bool preset_store_save(const preset_bank_t *bank);`
  - These are the only functions in the preset path that touch Zephyr, so they are excluded from `SRC_presets` in the host Makefile.

- [ ] **Step 1: Write `firmware/src/presets_flash.c`**

Use the flash map API against the `storage` partition declared in the board devicetree (`stem_player.dts`, `storage_partition` at `0xFF000`, length `0x1000`).

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
            ok = (flash_area_write(fa, (off_t)off, rec, sizeof(rec)) == 0);
        }
    }

    flash_area_close(fa);
    return ok;
}
```

Add both prototypes to `presets.h` under a comment marking them as the Zephyr-only part of the module.

- [ ] **Step 2: Load presets at boot in `main.c`**

After `button_engine_init`, call `preset_store_load` and, on success, push each slot into the engine with `button_engine_set_preset`.

- [ ] **Step 3: Handle the save action**

Replace the `TODO(Phase 7)` branch. On `BUTTON_ACTION_SAVE_PRESET`:

```c
/* Snapshot what the puck last SENT, which is all it knows: MIDI here is
 * one-way, so tweaks made from Push 3 or the synth's own menu are not
 * captured. Accepted in the design. Freeze is deliberately excluded. */
static int snapshot_surface(const profile_t *prof, cc_msg_t *out, int out_max)
{
    int n = 0;
    for (int c = 0; c < prof->preset_capture_len && n < out_max; c++) {
        uint8_t cc = prof->preset_capture[c];
        for (int f = 0; f < PROFILE_NUM_FADERS; f++) {
            if (prof->fader[f].cc == cc && g_fader[f].have_sent) {
                out[n++] = (cc_msg_t){ prof->fader[f].channel, cc,
                                       g_fader[f].last_sent };
            }
        }
        for (int b = 0; b < PROFILE_NUM_BUTTONS; b++) {
            const button_cfg_t *cfg = &prof->button[b];
            if (cfg->mode == BTN_MODE_STEP && cfg->cc == cc) {
                out[n++] = (cc_msg_t){ cfg->channel, cc,
                                       cfg->steps[button_engine_step_index(&eng, b)] };
            }
        }
    }
    return n;
}
```

Then `button_engine_set_preset(&eng, slot, snap, n)`, persist the whole bank with `preset_store_save`, and confirm on the LEDs: blink that button's track LED three times fast. A failed write must blink differently (a single long blink) so a silent failure is impossible to miss.

- [ ] **Step 4: Build, flash and test the full gesture**

Expected sequence at the rack:
1. Set the four faders somewhere musical, set shimmer to step 2.
2. Hold track 3 for two seconds: the LED confirms with three fast blinks.
3. Move every fader somewhere else.
4. Tap track 3: all four parameters glide back to the stored scene (the synth's own CC smoothing turns the burst into a morph, which is the whole reason presets are CC bursts rather than a new message type).
5. Power the puck off, power it back on, tap track 3 again: the same scene replays. This is the persistence test.
6. Reflash the firmware, power on, tap track 3: the scene is still there, because the flasher stops below `0xFF000`. Confirm it.

- [ ] **Step 5: Commit**

```bash
git add firmware/src
git commit -m "feat: presets persist to the storage page and survive reflashing"
```

---

# Phase 8: Session validation and release

### Task 8.1: The session that decides the stop rule

**Files:**
- Create: `docs/session-log.md`
- Modify: `README.md`

The spec's stop rule: if after one real session the puck does not beat Push 3 for performing these four gestures, the firmware is parked and the puck returns to stock. This task executes that test honestly.

- [ ] **Step 1: Play one real rack session**

String synth, puck on the desk, Push 3 pushed out of reach. Ride cutoff, reverb wet, delay time and delay feedback from the faders. Use freeze as a pedal at least twice. Step shimmer at least once. Store and recall both presets.

- [ ] **Step 2: Write down what actually happened**

In `docs/session-log.md`: what worked, what felt wrong, what was reached for and not found, any stuck or missed message, whether fader resolution felt sufficient at 7 bits, whether the 10 ms rate limit was audible as stepping. Be specific. Vague notes here waste the next session.

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

Faders move their parameters, freeze works both ways, shimmer steps, presets recall. Do not skip this: the last build tested is the one that ships.

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
- **MIDI clock output.** The lifted TX already carries the code path; it needs a tempo source. Only worth it if the synth-side delay clock-sync work lands.
- **Function button as a shift layer** for a second CC bank, which would give palette cycling (CC 43) a home. YAGNI until v1 has been performed with.
- **Renode emulation** via `softmodded/spire`, for firmware iteration without touching scarce hardware. Worth standing up only if hardware access becomes the bottleneck.

## Risks carried into execution

- **The TRS MIDI TX is not proven in writing.** Mitigated by Phase 0 preceding the bench, so a polarity flip is a rebuild rather than a dead session, and by Task 1.3's decision tree.
- **The pucks are unreleased prototypes.** Mitigated by developing on one, by the recovery drill in Task 1.1, and by the BIG FIVE constraints being restated at the top of `main.c`.
- **Touch-fader jitter becoming CC zipper.** Mitigated by the deadband, the rate limit and the synth's own CC smoothing. The idle-flicker check in Task 6.1 is the canary.
- **A missed button release leaving freeze stuck on.** Mitigated by the three-pass debounce and by Task 5.3's explicit fast-tap test. This is the single most session-ruining failure mode in the design.
