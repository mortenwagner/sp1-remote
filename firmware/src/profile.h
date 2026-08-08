/* PURE. The whole control surface in one table.
 *
 * This firmware is a generic 4-fader / 4-button MIDI controller. The table
 * below is the shipped default profile, which happens to target the
 * PopGoblin string synth. Changing the surface means changing this table
 * and nothing else. A browser-based editor over WebSerial that rewrites
 * this table at runtime is parked for v1.1.
 *
 * BTN_MODE_LIST and the per-button list[] exist because the frozen spec
 * requires "per-button CC list + channel" in the config table. */
#ifndef SP1_PROFILE_H
#define SP1_PROFILE_H

#include <stdint.h>
#include "cc_msg.h"

#define PROFILE_NUM_FADERS   4
#define PROFILE_NUM_BUTTONS  4
#define PROFILE_MAX_STEPS    4
#define PROFILE_MAX_CAPTURE  5
#define PROFILE_MAX_BTN_LIST 5

typedef enum {
    BTN_MODE_TOGGLE  = 0,
    BTN_MODE_CYCLE   = 1,
    BTN_MODE_PRESET  = 2,
    /* Emit a fixed list of (cc, value) pairs configured in the table. This
     * is what makes the surface generic: any button can be any list. */
    BTN_MODE_LIST    = 3,
} btn_mode_t;

typedef struct {
    uint8_t cc;
    uint8_t channel;
} fader_cfg_t;

typedef struct {
    btn_mode_t mode;
    uint8_t    channel;
    uint8_t    cc;
    uint8_t    on_value;
    uint8_t    off_value;
    uint8_t    steps[PROFILE_MAX_STEPS];
    uint8_t    n_steps;
    uint8_t    init_step;
    uint8_t    preset_slot;
    cc_msg_t   list[PROFILE_MAX_BTN_LIST];
    uint8_t    n_list;
} button_cfg_t;

typedef struct {
    fader_cfg_t  fader[PROFILE_NUM_FADERS];
    button_cfg_t button[PROFILE_NUM_BUTTONS];
    uint8_t      preset_capture[PROFILE_MAX_CAPTURE];
    uint8_t      preset_capture_len;
} profile_t;

extern const profile_t profile_popgoblin_default;

#endif /* SP1_PROFILE_H */
