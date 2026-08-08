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

## The mapping editor (in monitor.html)

Live now. Connect, and the bottom section reads the puck's current mapping
and lets you change any control's CC and channel. Edits apply immediately in
RAM; **save to flash** persists them, **defaults** restores the shipped
mapping in RAM only so nothing is lost until you save.

The wire protocol is JSON lines over the same CDC console, shaped after
feldd's: one object per line each way, replies always `{"t":"ok"}`,
`{"t":"err"}` or a `_r` response, so the page never has to guess whether a
command applied. It is plain enough to drive by hand:

```
{"t":"prof"}                      read the mapping
{"t":"set","f":0,"cc":20,"ch":1}  fader 0 -> CC 20, channel 2 on the wire
{"t":"save"}                      persist
{"t":"default"}                   back to the compiled mapping (RAM only)
{"t":"quiet","on":1}              stop the diagnostic stream
```

Channels are 0-15 on the wire and shown 1-16 in the page, as musicians count.

The editor silences the diagnostic stream on connect, so if you have `screen`
open at the same time you will see the puck go quiet. That is the `quiet`
command, not a hang.

## Parked: what the editor does not do yet

The v1.1 plan is a browser editor for the control surface: any CC, any
channel, per control, written over WebSerial. This page is deliberately
the same plumbing, so the editor becomes a tab here rather than a new
thing.

Button MODE is read-only in the page: you can retarget a button's CC and
channel, but not turn a toggle into a cycle, or edit a cycle's step values
or a list's contents. The firmware's profile record already stores all of
it, so this is a UI gap rather than a firmware one.

Nor does it do feldd's larger ideas: multiple profiles, layers, keyboard
output, or naming controls. Those are worth stealing if this gets used.
