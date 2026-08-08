/* TRS MIDI transmit on the SP-1 sync jack. One of two sinks; the queue in
 * midi_tx.c fans out to this and to USB MIDI.
 *
 * The sync jack's ring is driven by P0.23 (BC807 base) through a PNP that
 * INVERTS the line, so the waveform is bit-banged rather than handed to a
 * UART peripheral. A hardware timer clocks one bit per ISR with interrupts
 * left on, which is what keeps it from starving anything else.
 *
 * Transplanted from sp1-tape-looper firmware/src/main.c:4419-4530 (MIT).
 * NOTE: that source calls its own MIDI TX untested on real gear. Nothing
 * here has been verified on a receiver yet: the sync jack is the one part
 * of this hardware with no published documentation, and the adapter it
 * needs is not built. See docs/hardware-notes.md. */
#ifndef SP1_MIDI_TRS_H
#define SP1_MIDI_TRS_H

#include <stdint.h>

void midi_trs_init(void);
void midi_trs_send_byte(uint8_t b);
void midi_trs_send_cc(uint8_t channel, uint8_t cc, uint8_t value);

#endif /* SP1_MIDI_TRS_H */
