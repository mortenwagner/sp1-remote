#include "txqueue.h"

void txq_init(txqueue_t *q)
{
    q->head  = 0;
    q->count = 0;
}

bool txq_push(txqueue_t *q, cc_msg_t m)
{
    for (uint8_t i = 0; i < q->count; i++) {
        uint8_t idx = (uint8_t)((q->head + i) % TXQ_MAX);
        if (q->item[idx].cc == m.cc && q->item[idx].channel == m.channel) {
            q->item[idx].value = m.value;
            return true;
        }
    }

    if (q->count >= TXQ_MAX) {
        return false;
    }

    uint8_t tail = (uint8_t)((q->head + q->count) % TXQ_MAX);
    q->item[tail] = m;
    q->count++;
    return true;
}

bool txq_pop(txqueue_t *q, cc_msg_t *out)
{
    if (q->count == 0) {
        return false;
    }
    *out    = q->item[q->head];
    q->head = (uint8_t)((q->head + 1) % TXQ_MAX);
    q->count--;
    return true;
}

uint8_t txq_count(const txqueue_t *q)
{
    return q->count;
}
