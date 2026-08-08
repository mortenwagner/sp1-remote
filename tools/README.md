# Browser tools

Self-contained pages, no build step and no external requests. Open them
directly, or serve the folder if your browser objects to `file://` and
WebSerial:

```sh
python3 -m http.server 8790 --directory tools
```

Then open `http://127.0.0.1:8790/monitor.html` in **Chrome or Edge**.
Safari does not implement WebSerial.

Note the CDC console can only be held by one program at a time: close
`screen` (and any other reader) before connecting here, or the port will
not open.

## monitor.html

A live view of the puck, parsed from the diagnostic line the firmware
already prints. Four fader bars with their CC numbers, the button row with
the raw ladder value, battery and charger state, and the MIDI transmit
counters.

Those counters are the useful part when something is wrong. `push`,
`drain` and `usbtx` should advance together; if `push` climbs while
`drain` stalls the transmit thread is stuck, and if `rdy` is 0 no host has
opened the MIDI interface so USB sends are being dropped. Working that out
from an empty MIDI capture took far longer than reading it off this page
would have.

## Parked: the profile editor

The v1.1 plan is a browser editor for the control surface: any CC, any
channel, per control, written over WebSerial. This page is deliberately
the same plumbing, so the editor becomes a tab here rather than a new
thing.

It is blocked on storage, not on the UI. `profile_popgoblin_default` is
`const`, compiled into the app image that the flasher overwrites, so a
runtime-editable profile has to live in the 4 KB page at `0xFF000` that
now holds presets. That needs a shared layout (a second record type in the
same append-log, latest wins), and it depends on that page being ours,
which `preset_store_is_safe()` and the PLAY dump exist to establish.

Settle the storage question first, then this becomes straightforward.
