/* TRS MIDI transmit on the SP-1 sync jack. One of two sinks; the queue in
 * midi_tx.c fans out to this and to USB MIDI.
 *
 * Data on the TIP at P0.20, TRS Type A, 31250 baud, driven by the UARTE1
 * peripheral. Follows bnjreece/feldd-sp1-firmware, which validated this
 * path against an OP-XY. Being Type A means a plain TRS cable reaches the
 * Tiliqua and a Midihub: no adapter needed. See docs/hardware-notes.md. */
#ifndef SP1_MIDI_TRS_H
#define SP1_MIDI_TRS_H

#include <stdint.h>

void midi_trs_init(void);
void midi_trs_send_byte(uint8_t b);
void midi_trs_send_cc(uint8_t channel, uint8_t cc, uint8_t value);

#endif /* SP1_MIDI_TRS_H */
