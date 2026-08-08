/* PURE. The unified button model: every button is "send a list of
 * (channel, cc, value)". Freeze is a one-item toggling list, shimmer is a
 * one-item cycling list, a preset is a short list replayed on the CCs the
 * faders already drive, and BTN_MODE_LIST is an arbitrary configured list.
 * No new CCs, and therefore no synth-side work. */
#ifndef SP1_BUTTONS_H
#define SP1_BUTTONS_H

#include <stdbool.h>
#include <stdint.h>
#include "controls.h"
#include "profile.h"

#define BUTTON_MAX_PRESET_SLOTS 2

/* Returned by button_engine_event instead of a message count. */
#define BUTTON_ACTION_SAVE_PRESET (-1)

typedef struct {
    cc_msg_t msg[PROFILE_MAX_CAPTURE];
    uint8_t  len;
} preset_slot_t;

typedef struct {
    const profile_t *prof;
    bool     latched[PROFILE_NUM_BUTTONS];    /* TOGGLE state */
    uint8_t  step_idx[PROFILE_NUM_BUTTONS];   /* CYCLE position */
    bool     save_armed[PROFILE_NUM_BUTTONS]; /* a hold consumed the release */
    int      pending_save_slot;
    preset_slot_t preset[BUTTON_MAX_PRESET_SLOTS];
} button_engine_t;

void button_engine_init(button_engine_t *e, const profile_t *prof);

/* Feed one button event mask from btn_update. Returns the number of
 * messages written to out, 0 for nothing to send, or
 * BUTTON_ACTION_SAVE_PRESET when the caller must snapshot the surface into
 * button_engine_pending_save_slot(e) and then call button_engine_set_preset.
 *
 * No duration parameter, deliberately. Nothing here reacts to HOW LONG a
 * button was held: TOGGLE and CYCLE act on the press, PRESET acts on the
 * HOLD event (which fires while the button is still down) or on the tap.
 * That is what makes a lost RELEASE harmless. */
int  button_engine_event(button_engine_t *e, int idx, uint8_t ev,
                         cc_msg_t *out, int out_max);

int  button_engine_pending_save_slot(const button_engine_t *e);
void button_engine_set_preset(button_engine_t *e, int slot,
                              const cc_msg_t *msgs, int len);
const preset_slot_t *button_engine_get_preset(const button_engine_t *e, int slot);

/* Re-sync a cycle button after a preset recall replayed its CC: pick the
 * step whose value is nearest what was just sent. Without this the puck's
 * step index, its LED, and the next press all disagree with the synth, and
 * the next snapshot captures the stale step. */
void button_engine_sync_cycle(button_engine_t *e, int idx, uint8_t value);

/* For the LED layer. */
uint8_t button_engine_step_index(const button_engine_t *e, int idx);
bool    button_engine_is_latched(const button_engine_t *e, int idx);

#endif /* SP1_BUTTONS_H */
