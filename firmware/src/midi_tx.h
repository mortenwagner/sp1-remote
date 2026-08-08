/* The transmit layer above both sinks.
 *
 * Everything outside Phase 3 calls midi_tx_send() and nothing else. It
 * pushes into a coalescing queue and returns immediately, so the control
 * loop never blocks on a wire. A drain thread pops and fans out to the TRS
 * jack and to USB MIDI.
 *
 * Why coalescing: four faders each emitting on every change would occupy a
 * large fraction of the 31250 baud wire, and popgoblin drains exactly one
 * MIDI FIFO entry per 5 ms timer ISR, so it absorbs at most 200 messages a
 * second. The queue keeps only the newest value per (channel, cc), which
 * makes a fast sweep cost one message per drain rather than one per
 * movement. */
#ifndef SP1_MIDI_TX_H
#define SP1_MIDI_TX_H

#include <stdbool.h>
#include <stdint.h>
#include "cc_msg.h"
#include "txqueue.h"

void midi_tx_init(void);
void midi_tx_send(cc_msg_t m);

/* Diagnostics, APPROXIMATE. These count attempts, not confirmed delivery:
 * a TRS byte can be dropped if the line is stuck, and the USB send result
 * is not checked. They are read without synchronisation. Their purpose is
 * to show which stage stopped advancing, which is what located the problem
 * when faders appeared to send nothing. */
/* Block until the queue is empty and the wire has gone idle, up to a
 * timeout. Call before a flash erase or write: on nRF52840 a word write
 * stalls the CPU for ~41 us and a page erase for ~85 ms, while one
 * bit-banged MIDI bit is 32 us, so a flash operation landing mid-byte
 * corrupts that byte's framing. Returns true if the queue drained. */
bool midi_tx_idle(uint32_t timeout_ms);

uint32_t midi_tx_pushed(void);
uint32_t midi_tx_drained(void);
uint32_t midi_tx_usb_sent(void);
bool     midi_tx_usb_ready(void);

#endif /* SP1_MIDI_TX_H */
