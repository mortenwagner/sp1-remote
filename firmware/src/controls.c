#include "controls.h"

uint8_t fader_raw_to_cc(int raw)
{
    if (raw <= 0) {
        return 0;
    }
    if (raw >= FADER_RAW_FULL) {
        return 127;
    }
    return (uint8_t)(((raw * 127) + (FADER_RAW_FULL / 2)) / FADER_RAW_FULL);
}

static int abs_diff(int a, int b)
{
    int d = a - b;
    return d < 0 ? -d : d;
}

bool fader_update(fader_state_t *st, int raw, uint8_t *out_value)
{
    if (raw < 0) {
        return false;
    }

    if (!st->have_seed) {
        st->have_seed = true;
        st->last_raw  = raw;
        return false;
    }

    if (abs_diff(raw, st->last_raw) < FADER_DEADBAND_RAW) {
        return false;
    }
    st->last_raw = raw;

    uint8_t v = fader_raw_to_cc(raw);

    /* Pickup is resolved BEFORE the duplicate-value check. Arming sets
     * last_sent to the recalled target, so a fader that lands exactly ON
     * the target would otherwise be rejected as a duplicate and stay armed
     * forever, blinking its LED while it is in fact already in agreement. */
    if (st->pickup_armed) {
        int side = (v > st->pickup_target) - (v < st->pickup_target);
        if (side != 0 && side == st->pickup_side) {
            return false;           /* still on the far side: suppressed */
        }
        st->pickup_armed = false;   /* crossed or landed on it: caught */
    }

    if (st->have_sent && v == st->last_sent) {
        return false;
    }

    st->last_sent = v;
    st->have_sent = true;
    *out_value    = v;
    return true;
}

void fader_arm_pickup(fader_state_t *st, uint8_t target)
{
    uint8_t cur = st->have_sent ? st->last_sent
                                : fader_raw_to_cc(st->last_raw);

    st->last_sent = target;
    st->have_sent = true;

    if (!st->have_seed || cur == target) {
        st->pickup_armed = false;
        return;
    }

    st->pickup_target = target;
    st->pickup_side   = (cur > target) ? 1 : -1;
    st->pickup_armed  = true;
}

bool fader_pickup_armed(const fader_state_t *st)
{
    return st->pickup_armed;
}

uint8_t btn_update(btn_state_t *st, bool pressed_now, uint32_t now_ms)
{
    uint8_t ev = 0;

    if (pressed_now && !st->down) {
        st->down       = true;
        st->hold_fired = false;
        st->down_at_ms = now_ms;
        return BTN_EV_PRESS;
    }

    if (pressed_now && st->down) {
        /* Unsigned subtraction, so the 32-bit millisecond wrap is a
         * non-event. Fires once per press. */
        if (!st->hold_fired &&
            (uint32_t)(now_ms - st->down_at_ms) >= BTN_HOLD_MS) {
            st->hold_fired = true;
            ev |= BTN_EV_HOLD;
        }
        return ev;
    }

    if (!pressed_now && st->down) {
        st->down = false;
        ev = BTN_EV_RELEASE;
        if (!st->hold_fired) {
            ev |= BTN_EV_TAP;
        }
        return ev;
    }

    return 0;
}
