/* TRS MIDI transmit on the SP-1 sync jack. One of two sinks; the queue in
 * midi_tx.c fans out to this and to USB MIDI.
 *
 * TRS Type A, 31250 baud: ring current source on P0.23 (low = on), data on
 * the tip sunk by P0.20 (UARTE1) and P0.17 (UARTE0) in step. A plain TRS
 * cable reaches the Tiliqua and a Midihub. See midi_trs.c and
 * docs/hardware-notes.md (2026-11-06). */
#ifndef SP1_MIDI_TRS_H
#define SP1_MIDI_TRS_H

#include <stdint.h>

void midi_trs_init(void);
void midi_trs_send_byte(uint8_t b);
void midi_trs_send_cc(uint8_t channel, uint8_t cc, uint8_t value);
/* Switch the ring current source off; call right before power-off. */
void midi_trs_release(void);

/* Program Change, one 2-byte DMA. */
void midi_trs_send_pc(uint8_t channel, uint8_t program);

#endif /* SP1_MIDI_TRS_H */
