#include <string.h>
#include "buttons.h"

void button_engine_init(button_engine_t *e, const profile_t *prof)
{
    memset(e, 0, sizeof(*e));
    e->prof = prof;

    for (int i = 0; i < PROFILE_NUM_BUTTONS; i++) {
        const button_cfg_t *cfg = &prof->button[i];
        /* Start a cycle button where the synth actually is, so the puck
         * agrees with it at power-on without transmitting. */
        if (cfg->mode == BTN_MODE_CYCLE && cfg->n_steps) {
            e->step_idx[i] = (uint8_t)(cfg->init_step % cfg->n_steps);
        }
        /* Factory-default preset content, overwritten later by whatever
         * flash holds. */
        if (cfg->mode == BTN_MODE_PRESET && cfg->n_list) {
            button_engine_set_preset(e, cfg->preset_slot, cfg->list,
                                     cfg->n_list);
        }
    }
    e->pending_save_slot = -1;
}

static int emit(cc_msg_t *out, int out_max, int n,
                uint8_t ch, uint8_t cc, uint8_t val)
{
    if (n >= out_max) {
        return n;
    }
    out[n].channel = ch;
    out[n].cc      = cc;
    out[n].value   = val;
    return n + 1;
}

int button_engine_event(button_engine_t *e, int idx, uint8_t ev,
                        cc_msg_t *out, int out_max)
{
    if (idx < 0 || idx >= PROFILE_NUM_BUTTONS) {
        return 0;
    }
    const button_cfg_t *cfg = &e->prof->button[idx];
    int n = 0;

    switch (cfg->mode) {

    case BTN_MODE_TOGGLE:
        /* Plain alternation on the press, and nothing at all on release.
         * The synth treats CC 64 as a level (>= 64 is on), so the puck
         * simply alternates between the two levels.
         *
         * Release is deliberately inert. An earlier design timed the
         * release to offer a momentary pedal as well, which meant a lost
         * RELEASE left freeze on for the rest of the session with no way
         * back. Here a lost PRESS costs one more press and nothing else,
         * and a dropped message re-syncs on the next press. */
        if (ev & BTN_EV_PRESS) {
            e->latched[idx] = !e->latched[idx];
            return emit(out, out_max, n, cfg->channel, cfg->cc,
                        e->latched[idx] ? cfg->on_value : cfg->off_value);
        }
        return 0;

    case BTN_MODE_CYCLE:
        /* Also on the press: a stepped parameter should move under the
         * finger, not when it leaves. */
        if (ev & BTN_EV_PRESS) {
            if (cfg->n_steps == 0) {
                return 0;
            }
            e->step_idx[idx] = (uint8_t)((e->step_idx[idx] + 1) % cfg->n_steps);
            return emit(out, out_max, n, cfg->channel, cfg->cc,
                        cfg->steps[e->step_idx[idx]]);
        }
        return 0;

    case BTN_MODE_LIST:
        if (ev & BTN_EV_PRESS) {
            for (int i = 0; i < cfg->n_list; i++) {
                n = emit(out, out_max, n, cfg->list[i].channel,
                         cfg->list[i].cc, cfg->list[i].value);
            }
            return n;
        }
        return 0;

    case BTN_MODE_PRESET:
        /* The one mode that MUST wait for the release, because a press
         * alone cannot yet be distinguished from the start of a
         * hold-to-save. The HOLD event fires while the button is still
         * down, so the save does not depend on a release either. */
        if (ev & BTN_EV_HOLD) {
            e->save_armed[idx]   = true;
            e->pending_save_slot = cfg->preset_slot;
            return BUTTON_ACTION_SAVE_PRESET;
        }
        if (ev & BTN_EV_RELEASE) {
            if (e->save_armed[idx]) {
                e->save_armed[idx] = false;  /* the hold already acted */
                return 0;
            }
            if (!(ev & BTN_EV_TAP)) {
                return 0;
            }
            int slot = cfg->preset_slot;
            if (slot < 0 || slot >= BUTTON_MAX_PRESET_SLOTS) {
                return 0;
            }
            const preset_slot_t *p = &e->preset[slot];
            for (int i = 0; i < p->len; i++) {
                n = emit(out, out_max, n, p->msg[i].channel,
                         p->msg[i].cc, p->msg[i].value);
            }
            return n;
        }
        return 0;
    }
    return 0;
}

int button_engine_pending_save_slot(const button_engine_t *e)
{
    return e->pending_save_slot;
}

void button_engine_set_preset(button_engine_t *e, int slot,
                              const cc_msg_t *msgs, int len)
{
    if (slot < 0 || slot >= BUTTON_MAX_PRESET_SLOTS) {
        return;
    }
    if (len < 0) {
        return;                 /* a negative length would make memcpy enormous */
    }
    if (len > PROFILE_MAX_CAPTURE) {
        len = PROFILE_MAX_CAPTURE;
    }
    memcpy(e->preset[slot].msg, msgs, (size_t)len * sizeof(cc_msg_t));
    e->preset[slot].len  = (uint8_t)len;
    e->pending_save_slot = -1;
}

const preset_slot_t *button_engine_get_preset(const button_engine_t *e, int slot)
{
    if (slot < 0 || slot >= BUTTON_MAX_PRESET_SLOTS) {
        return NULL;
    }
    return &e->preset[slot];
}

void button_engine_sync_cycle(button_engine_t *e, int idx, uint8_t value)
{
    if (idx < 0 || idx >= PROFILE_NUM_BUTTONS) {
        return;
    }
    const button_cfg_t *cfg = &e->prof->button[idx];
    if (cfg->mode != BTN_MODE_CYCLE || cfg->n_steps == 0) {
        return;
    }

    uint8_t best = 0;
    int best_d = 256;
    for (uint8_t i = 0; i < cfg->n_steps; i++) {
        int d = (int)cfg->steps[i] - (int)value;
        if (d < 0) {
            d = -d;
        }
        if (d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    e->step_idx[idx] = best;
}

uint8_t button_engine_step_index(const button_engine_t *e, int idx)
{
    return (idx >= 0 && idx < PROFILE_NUM_BUTTONS) ? e->step_idx[idx] : 0u;
}

bool button_engine_is_latched(const button_engine_t *e, int idx)
{
    return (idx >= 0 && idx < PROFILE_NUM_BUTTONS) ? e->latched[idx] : false;
}
