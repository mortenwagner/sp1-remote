#include "controls.h"
#include "test_util.h"

static void test_raw_to_cc_endpoints(void)
{
    CHECK_EQ(fader_raw_to_cc(0), 0);
    CHECK_EQ(fader_raw_to_cc(FADER_RAW_FULL), 127);
    CHECK_EQ(fader_raw_to_cc(FADER_RAW_FULL + 500), 127);
    CHECK_EQ(fader_raw_to_cc(-1), 0);
}

static void test_raw_to_cc_midpoint(void)
{
    CHECK_EQ(fader_raw_to_cc(FADER_RAW_FULL / 2), 64);
}

/* The puck must be silent at power-on: it has no idea what the synth is
 * set to, and blasting four CCs from wherever the faders happen to sit
 * would stamp on the current patch. The first reading only seeds. */
static void test_first_reading_seeds_silently(void)
{
    fader_state_t st = {0};
    uint8_t v = 0xFF;
    CHECK(!fader_update(&st, 2000, &v));
    CHECK_EQ(v, 0xFF);
}

static void test_first_real_move_emits(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 2000, &v));
    CHECK(fader_update(&st, 2100, &v));
    CHECK_EQ(v, fader_raw_to_cc(2100));
}

static void test_adc_error_never_emits(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, -1, &v));
    CHECK(!st.have_seed);
}

static void test_jitter_inside_deadband_is_ignored(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 1000, &v));
    CHECK(!fader_update(&st, 1004, &v));
    CHECK(!fader_update(&st, 996,  &v));
}

/* A 7-bit step spans about 29 raw counts. At the measured full scale of
 * 3730, CC 34 covers raw 984 to 1013, so a move of 20 counts inside that
 * window clears the 12-count deadband and still lands on the same CC. That
 * is the only way to reach the duplicate-suppression branch: a smaller move
 * is rejected by the deadband first and never gets there. */
static void test_same_cc_value_is_not_resent(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 975, &v));     /* seed, silent */
    CHECK(fader_update(&st, 990, &v));      /* emits CC 34 */
    CHECK_EQ(v, 34);
    CHECK_EQ(fader_raw_to_cc(1010), 34);    /* same bucket... */
    CHECK(!fader_update(&st, 1010, &v));    /* ...so nothing is resent */
}

/* --- soft pickup after a preset recall --- */

static void test_pickup_suppresses_until_the_fader_crosses(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 300, &v));          /* seed low */
    CHECK(fader_update(&st, 400, &v));           /* now sending, cc ~14 */

    fader_arm_pickup(&st, 100);                  /* recall put the synth at 100 */
    CHECK(fader_pickup_armed(&st));

    /* Moving up but still well below 100: nothing goes out. */
    CHECK(!fader_update(&st, 1000, &v));
    CHECK(!fader_update(&st, 2000, &v));
    CHECK(fader_pickup_armed(&st));

    /* Crossing the recalled value catches it and takes over. */
    CHECK(fader_update(&st, 3000, &v));
    CHECK(!fader_pickup_armed(&st));
    CHECK_EQ(v, fader_raw_to_cc(3000));
}

static void test_pickup_not_armed_when_already_at_the_target(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 1000, &v));
    CHECK(fader_update(&st, 1100, &v));
    fader_arm_pickup(&st, st.last_sent);
    CHECK(!fader_pickup_armed(&st));
}

static void test_pickup_from_above_also_catches(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 3600, &v));
    CHECK(fader_update(&st, 3500, &v));
    fader_arm_pickup(&st, 40);
    CHECK(fader_pickup_armed(&st));
    CHECK(!fader_update(&st, 2000, &v));         /* cc ~69, still above 40 */
    CHECK(fader_update(&st, 900, &v));           /* cc ~31, crossed below */
    CHECK(!fader_pickup_armed(&st));
}

/* Stopping exactly ON the recalled value is "caught" too. Arming adopts the
 * target as last_sent, so without resolving pickup before the duplicate
 * check this fader would stay armed forever, blinking its LED while already
 * in agreement with the synth. */
static void test_pickup_landing_exactly_on_target_disarms(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 300, &v));
    CHECK(fader_update(&st, 400, &v));

    uint8_t target = fader_raw_to_cc(2000);
    fader_arm_pickup(&st, target);
    CHECK(fader_pickup_armed(&st));

    /* Land precisely on it: nothing to send, but pickup must let go. */
    CHECK(!fader_update(&st, 2000, &v));
    CHECK(!fader_pickup_armed(&st));

    /* And the fader is live again immediately afterwards. */
    CHECK(fader_update(&st, 2400, &v));
}

static void test_arming_pickup_adopts_the_recalled_value(void)
{
    fader_state_t st = {0};
    uint8_t v = 0;
    CHECK(!fader_update(&st, 1000, &v));
    CHECK(fader_update(&st, 1100, &v));
    fader_arm_pickup(&st, 77);
    CHECK_EQ(st.last_sent, 77);
}

/* --- buttons --- */

static void test_press_then_short_release_is_a_tap(void)
{
    btn_state_t st = {0};
    CHECK_EQ(btn_update(&st, true, 0), BTN_EV_PRESS);
    CHECK_EQ(btn_update(&st, true, 50), 0);
    CHECK_EQ(btn_update(&st, false, 120), BTN_EV_RELEASE | BTN_EV_TAP);
}

static void test_hold_fires_once_and_release_is_not_a_tap(void)
{
    btn_state_t st = {0};
    CHECK_EQ(btn_update(&st, true, 0), BTN_EV_PRESS);
    CHECK_EQ(btn_update(&st, true, BTN_HOLD_MS - 1), 0);
    CHECK_EQ(btn_update(&st, true, BTN_HOLD_MS), BTN_EV_HOLD);
    CHECK_EQ(btn_update(&st, true, BTN_HOLD_MS + 500), 0);
    CHECK_EQ(btn_update(&st, false, BTN_HOLD_MS + 600), BTN_EV_RELEASE);
}

/* The hold fires while the button is still DOWN, which is why the save
 * gesture does not depend on a release arriving either. */
static void test_hold_fires_while_still_held(void)
{
    btn_state_t st = {0};
    CHECK_EQ(btn_update(&st, true, 1000), BTN_EV_PRESS);
    CHECK_EQ(btn_update(&st, true, 1000 + BTN_HOLD_MS - 1), 0);
    CHECK_EQ(btn_update(&st, true, 1000 + BTN_HOLD_MS), BTN_EV_HOLD);
}

/* The control loop's uptime counter is 32-bit milliseconds, which wraps
 * after about 49 days. Unsigned subtraction makes the wrap a non-event,
 * but only if nobody compares timestamps directly. */
static void test_hold_detection_survives_the_millisecond_wrap(void)
{
    btn_state_t st = {0};
    uint32_t near_wrap = 0xFFFFFF00u;
    CHECK_EQ(btn_update(&st, true, near_wrap), BTN_EV_PRESS);
    CHECK_EQ(btn_update(&st, true, near_wrap + 100u), 0);
    CHECK_EQ(btn_update(&st, true, near_wrap + BTN_HOLD_MS), BTN_EV_HOLD);
}

static void test_idle_button_reports_nothing(void)
{
    btn_state_t st = {0};
    CHECK_EQ(btn_update(&st, false, 0), 0);
    CHECK_EQ(btn_update(&st, false, 5000), 0);
}

int main(void)
{
    RUN(test_raw_to_cc_endpoints);
    RUN(test_raw_to_cc_midpoint);
    RUN(test_first_reading_seeds_silently);
    RUN(test_first_real_move_emits);
    RUN(test_adc_error_never_emits);
    RUN(test_jitter_inside_deadband_is_ignored);
    RUN(test_same_cc_value_is_not_resent);
    RUN(test_pickup_suppresses_until_the_fader_crosses);
    RUN(test_pickup_not_armed_when_already_at_the_target);
    RUN(test_pickup_from_above_also_catches);
    RUN(test_pickup_landing_exactly_on_target_disarms);
    RUN(test_arming_pickup_adopts_the_recalled_value);
    RUN(test_press_then_short_release_is_a_tap);
    RUN(test_hold_fires_once_and_release_is_not_a_tap);
    RUN(test_hold_fires_while_still_held);
    RUN(test_hold_detection_survives_the_millisecond_wrap);
    RUN(test_idle_button_reports_nothing);
    TEST_MAIN_END();
}
