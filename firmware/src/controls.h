/* PURE LOGIC. No Zephyr headers may ever appear in this file: it is
 * compiled into the host unit tests as well as into the firmware. */
#ifndef SP1_CONTROLS_H
#define SP1_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

#define FADER_RAW_FULL          3700
#define FADER_DEADBAND_RAW      8

typedef struct {
    int      last_raw;
    uint8_t  last_sent;
    bool     have_seed;
    bool     have_sent;
    bool     pickup_armed;
    uint8_t  pickup_target;
    int8_t   pickup_side;
} fader_state_t;

uint8_t fader_raw_to_cc(int raw);
bool fader_update(fader_state_t *st, int raw, uint8_t *out_value);
void fader_arm_pickup(fader_state_t *st, uint8_t target);
bool fader_pickup_armed(const fader_state_t *st);

/* Hold-to-save a preset. The ONLY duration this firmware measures, and it
 * fires while the button is still down, so nothing depends on a release
 * arriving. */
#define BTN_HOLD_MS        2000

#define BTN_EV_PRESS    0x01u
#define BTN_EV_HOLD     0x02u
#define BTN_EV_TAP      0x04u
#define BTN_EV_RELEASE  0x08u

typedef struct {
    bool     down;
    bool     hold_fired;
    uint32_t down_at_ms;
} btn_state_t;

/* Feed the debounced pressed/released state once per control pass. */
uint8_t btn_update(btn_state_t *st, bool pressed_now, uint32_t now_ms);

#endif /* SP1_CONTROLS_H */
