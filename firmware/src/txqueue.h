/* PURE. The single coalescing transmit queue.
 *
 * Every message the firmware sends goes through here. Pushing a (channel,
 * cc) that is already pending overwrites its value IN PLACE and keeps its
 * position, so a fast fader sweep collapses to one message per drain
 * without ever pushing a waiting button message to the back.
 *
 * NOT thread-safe by itself: the control loop pushes and the transmit
 * thread pops, so both must hold the same mutex (see midi_tx.c). Keeping
 * the locking out here is what allows host testing. */
#ifndef SP1_TXQUEUE_H
#define SP1_TXQUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include "cc_msg.h"

#define TXQ_MAX 16

typedef struct {
    cc_msg_t item[TXQ_MAX];
    uint8_t  head;
    uint8_t  count;
} txqueue_t;

void    txq_init(txqueue_t *q);
bool    txq_push(txqueue_t *q, cc_msg_t m);
bool    txq_pop(txqueue_t *q, cc_msg_t *out);
uint8_t txq_count(const txqueue_t *q);

#endif /* SP1_TXQUEUE_H */
