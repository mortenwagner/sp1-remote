/* PURE LOGIC. No Zephyr headers may ever appear in this file: it is
 * compiled into the host unit tests as well as into the firmware. */
#ifndef SP1_CONTROLS_H
#define SP1_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

/* Full-scale raw code of a fader on the 12-bit SAADC as configured in
 * app.overlay. MEASURED ON POP 2026-08-08 by sweeping all four strips:
 * they reach 3742, 3735, 3735 and 3734. The looper's 3700 (main.c:7487)
 * would clamp the top ~1% of every strip's travel to 127. 3730 sits just
 * below the lowest observed maximum, so every fader can still reach 127. */
#define FADER_RAW_FULL          3730
/* Raw counts a fader must move past its last accepted position before a new
 * value is considered.
 *
 * MEASURED ON POP: resting jitter is 4 to 7 counts peak-to-peak in a quiet
 * window, and up to 23 over tens of seconds on a strip parked near its rail.
 * That is far worse than the plus-or-minus 1 the looper's source claims, and
 * it is why this is 12 rather than 8. One 7-bit step is about 29 counts, so
 * 12 stays under half a step and costs no perceptible resolution.
 *
 * Note the deadband is not what prevents repeat sends (the duplicate-value
 * check does that); it stops noise reaching the arithmetic at all. The
 * acceptance test is Phase 4's: a puck left alone must emit nothing. */
#define FADER_DEADBAND_RAW      12

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
