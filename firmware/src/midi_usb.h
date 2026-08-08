/* USB MIDI 2 sink.
 *
 * The second of two sinks. This one cannot reach the string synth:
 * popgoblin instantiates only a TRS serial receiver and has no USB MIDI
 * host (polysyn has one, popgoblin does not). Its job is to make the whole
 * control surface testable at a desk with one USB-C cable, instead of every
 * behaviour test depending on the undocumented sync jack and a hand-made
 * adapter. Later it also lets the puck drive a Mac.
 *
 * Send path lifted from ericlewis/sp1-midi app/MidiController.cpp:12-18
 * (MIT), transliterated from C++. */
#ifndef SP1_MIDI_USB_H
#define SP1_MIDI_USB_H

#include <stdbool.h>
#include <stdint.h>

void midi_usb_init(void);
void midi_usb_send_cc(uint8_t channel, uint8_t cc, uint8_t value);
bool midi_usb_ready(void);

#endif /* SP1_MIDI_USB_H */
