#include "perform.h"

static const uint8_t hold_cc[PERFORM_NUM_HOLDS] = {
    PERFORM_CC_STUTTER_8, PERFORM_CC_STUTTER_16,
};

void perform_init(perform_t *p, uint8_t channel)
{
    *p = (perform_t){ .channel = channel };
}

int perform_rocker(perform_t *p, int dir, cc_msg_t *out)
{
    int s = (int)p->song + (dir < 0 ? -1 : 1);

    if (s < 0) {
        s = PERFORM_NUM_SONGS - 1;
    } else if (s >= PERFORM_NUM_SONGS) {
        s = 0;
    }
    p->song = (uint8_t)s;
    out[0] = (cc_msg_t){ p->channel, CC_MSG_PROGRAM, p->song };
    return 1;
}

int perform_play(perform_t *p, uint32_t now_ms, cc_msg_t *out)
{
    p->flash_until = now_ms + PERFORM_FLASH_MS;
    out[0] = (cc_msg_t){ p->channel, PERFORM_CC_NEXT_CHORD, 127 };
    return 1;
}

int perform_hold(perform_t *p, int which, bool down, uint32_t now_ms,
                 cc_msg_t *out)
{
    if (which < 0 || which >= PERFORM_NUM_HOLDS) {
        return 0;
    }
    uint8_t cc = hold_cc[which];

    if (down && !p->held[which]) {
        p->held[which] = true;
        p->last_ms[which] = now_ms;
        out[0] = (cc_msg_t){ p->channel, cc, 127 };
        return 1;
    }
    if (down && (uint32_t)(now_ms - p->last_ms[which]) >= PERFORM_HOLD_REPEAT_MS) {
        p->last_ms[which] = now_ms;
        out[0] = (cc_msg_t){ p->channel, cc, 127 };
        return 1;
    }
    if (!down && p->held[which]) {
        p->held[which] = false;
        out[0] = (cc_msg_t){ p->channel, cc, 0 };
        return 1;
    }
    return 0;
}

bool perform_is_held(const perform_t *p, int which)
{
    return which >= 0 && which < PERFORM_NUM_HOLDS && p->held[which];
}

uint8_t perform_side_code(const perform_t *p)
{
    return (uint8_t)(p->song + 1u);
}

bool perform_flash(const perform_t *p, uint32_t now_ms)
{
    return (int32_t)(p->flash_until - now_ms) > 0;
}
