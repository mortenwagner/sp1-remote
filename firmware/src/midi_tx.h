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

#include "cc_msg.h"
#include "txqueue.h"

void midi_tx_init(void);
void midi_tx_send(cc_msg_t m);

#endif /* SP1_MIDI_TX_H */
