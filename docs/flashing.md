# Flashing and recovery

Executed on the dev puck **pop**, 2026-08-08. Task 1.1 of the plan: PASSED.

## The dev puck: pop

| | |
|---|---|
| Hardware serial (reported by app firmware) | `12EA9EE0D860060B` |
| Serial port in bootloader mode | `/dev/tty.usbmodemF5CD10918B02*` |
| Serial port running the looper | `/dev/tty.usbmodem11422201` |

No other puck gets connected during this project.

## Telling which mode pop is in, from the Mac

This is the most reliable check, and it does not depend on the flasher page:

```sh
ioreg -p IOUSB -l -w 0 | grep kUSBProductString | grep -i "stem\|SP-1"
```

| USB product string | What is running |
|---|---|
| `stem player` | the TE bootloader |
| `SP-1 Audio` (vendor `softmodded`, PID 0x5210) | the tape looper |

`system_profiler SPUSBDataType` returns nothing under the agent sandbox; use
`ioreg` instead.

## Entering the bootloader

1. Power pop off.
2. Hold **Track 1 + Track 4**.
3. Plug in USB-C while still holding.
4. Release once the **Track 1 LED lights**.

Works from a powered-off device and from one running custom firmware. Both
were verified.

## Flashing

1. Serve the local mirror of the flasher:
   ```sh
   cd "/Users/morten/Documents/Other Creations/dev/solderless/solderless-2026-05-18"
   python3 -m http.server 8788
   ```
2. Open `http://127.0.0.1:8788/` in **Chrome or Edge** (WebSerial does not
   exist in Safari).
3. **firmware utility** → choose file → connect → flash.
4. `refs/sp1-tape-looper/sp1_looper.bin` reports as *105.7 kb, 27 pages*.
   If the size differs, it is the wrong file: stop.

## Two gotchas that look exactly like failure

**1. The serial port changes when the mode changes, and the page does not
notice.** Pop enumerates with a different USB identity and a different
`/dev/tty.usbmodem*` in bootloader mode than when running an app. After any
reboot the flasher is still holding the previous port, so its next query
returns nonsense and `device mode` reads `unknown (...)` instead of
`boot mode`. **Reload the page (⌘R) and reconnect after every mode change.**
This cost us a scare mid-drill: it reads as a failed recovery when the
recovery in fact worked.

**2. In boot mode, device info shows dashes for every runtime field.** That
is correct. The digit string it prints decodes from ASCII to
**"unknown command 0x5c"**: the page is asking for runtime state
(temperature, charging, faders, ladders) and the bootloader does not
implement that query. Those fields only populate when application firmware
is running. Two useful signals hide in there: the response proves the link
works, and the absence of a mode byte proves you are talking to the
bootloader.

## Recovery drill result

- Bootloader entry from stock firmware: **PASS**
- Flash the looper: **PASS** (rebooted, enumerated as `SP-1 Audio`)
- Bootloader re-entry from running custom firmware: **PASS**, confirmed
  three independent ways (Track 1 LED, USB identity reverting to
  `stem player`, and `boot mode` on the page after reconnecting)

A second flash-and-re-enter cycle was deliberately skipped. Flashing always
happens in bootloader mode, where the bootloader erases and rewrites the app
slot; whether the previous occupant was stock or custom does not change that
operation, so a repeat exercises the same path with the same inputs.

## Still untested, and it cannot be tested yet

Recovery from a **wedged** app, as distinct from a healthy one. That is what
the Track 1 + Track 4 escape hatch inside our own firmware is for
(`enter_dfu`, transplanted in Task 2.1, wired in Task 5.2 Step 1). It cannot
be exercised until there is firmware of ours to wedge. Task 5.2's rack test
covers it, including the check that the combo does not instead fire a preset
save.

## If it will not enter bootloader

Nothing in this drill required it, but for the future: check the USB product
string first with the `ioreg` command above. If it says `stem player`, pop is
already in the bootloader and the problem is the page, not the device.
