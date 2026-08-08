#include "buttons.h"
#include "profile.h"
#include "test_util.h"

#define OUT_MAX 8

static button_engine_t eng;
static cc_msg_t out[OUT_MAX];

static void setup(void)
{
    button_engine_init(&eng, &profile_popgoblin_default);
}

/* --- freeze: a plain toggle on the press --- */

static void test_freeze_press_turns_on(void)
{
    setup();
    int n = button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].cc, 64);
    CHECK_EQ(out[0].value, 127);
    CHECK(button_engine_is_latched(&eng, 0));
}

static void test_freeze_second_press_turns_off(void)
{
    setup();
    button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);
    int n = button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].value, 0);
    CHECK(!button_engine_is_latched(&eng, 0));
}

/* THE POINT OF THE TOGGLE DESIGN: release is inert, so a release that is
 * late, early, or never delivered at all cannot strand freeze in the on
 * state. The earlier timed-release design could, and that was the single
 * most session-ruining failure mode in the surface. */
static void test_freeze_ignores_release_entirely(void)
{
    setup();
    button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);

    CHECK_EQ(button_engine_event(&eng, 0, BTN_EV_RELEASE, out, OUT_MAX), 0);
    CHECK_EQ(button_engine_event(&eng, 0, BTN_EV_RELEASE | BTN_EV_TAP,
                                 out, OUT_MAX), 0);
    CHECK_EQ(button_engine_event(&eng, 0, BTN_EV_HOLD, out, OUT_MAX), 0);
    /* Still on, whatever the release did or did not do. */
    CHECK(button_engine_is_latched(&eng, 0));
}

/* A press whose release is lost still toggles correctly next time: the
 * state machine only ever advances on a press. */
static void test_freeze_survives_a_lost_release(void)
{
    setup();
    button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);   /* on */
    /* release lost entirely: no event delivered at all */
    int n = button_engine_event(&eng, 0, BTN_EV_PRESS, out, OUT_MAX);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].value, 0);                                  /* off */
}

/* --- shimmer: each press steps --- */

static void test_shimmer_steps_and_wraps(void)
{
    setup();
    const uint8_t want[5] = { 16, 112, 80, 48, 16 };
    for (int i = 0; i < 5; i++) {
        int n = button_engine_event(&eng, 1, BTN_EV_PRESS, out, OUT_MAX);
        CHECK_EQ(n, 1);
        CHECK_EQ(out[0].cc, 105);
        CHECK_EQ(out[0].value, want[i]);
    }
}

static void test_shimmer_release_sends_nothing(void)
{
    setup();
    CHECK_EQ(button_engine_event(&eng, 1, BTN_EV_RELEASE | BTN_EV_TAP,
                                 out, OUT_MAX), 0);
}

/* PopGoblin boots with shimmer at Full. The puck must start on that same
 * step so its first press is a real change and its LED does not lie, and
 * it must reach that agreement WITHOUT transmitting. */
static void test_cycle_starts_on_the_synths_default_step(void)
{
    setup();
    CHECK_EQ(button_engine_step_index(&eng, 1), 2);
}

/* A recall replays CC 105, which moves shimmer on the synth. If the puck
 * does not re-sync, its LED lies and the next press jumps from a stale
 * position, and a later snapshot captures the stale step. */
static void test_recall_resyncs_the_cycle_step(void)
{
    setup();
    CHECK_EQ(button_engine_step_index(&eng, 1), 2);      /* Full at boot */

    button_engine_sync_cycle(&eng, 1, 112);              /* recall sent Off */
    CHECK_EQ(button_engine_step_index(&eng, 1), 0);

    /* Nearest-match, not exact-match: a value between buckets still lands
     * on the step the synth will have chosen. */
    button_engine_sync_cycle(&eng, 1, 50);               /* nearest 48 = Full */
    CHECK_EQ(button_engine_step_index(&eng, 1), 2);

    /* And the next press steps on from there, not from the stale index. */
    int n = button_engine_event(&eng, 1, BTN_EV_PRESS, out, OUT_MAX);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].value, 16);                          /* Full -> Boost */
}

static void test_sync_cycle_ignores_non_cycle_buttons(void)
{
    setup();
    button_engine_sync_cycle(&eng, 0, 99);   /* freeze is a toggle */
    button_engine_sync_cycle(&eng, 9, 99);   /* out of range */
    CHECK(!button_engine_is_latched(&eng, 0));
}

/* --- presets: the only mode that waits for a release --- */

static void test_preset_tap_replays_the_stored_list(void)
{
    setup();
    cc_msg_t scene[3] = {
        { 0, 102, 30 }, { 0, 104, 90 }, { 0, 107, 12 },
    };
    button_engine_set_preset(&eng, 0, scene, 3);
    int n = button_engine_event(&eng, 2, BTN_EV_RELEASE | BTN_EV_TAP,
                                out, OUT_MAX);
    CHECK_EQ(n, 3);
    CHECK_EQ(out[0].cc, 102);
    CHECK_EQ(out[0].value, 30);
    CHECK_EQ(out[2].cc, 107);
    CHECK_EQ(out[2].value, 12);
}

static void test_preset_press_alone_does_not_replay(void)
{
    setup();
    cc_msg_t scene[1] = { { 0, 102, 30 } };
    button_engine_set_preset(&eng, 0, scene, 1);
    CHECK_EQ(button_engine_event(&eng, 2, BTN_EV_PRESS, out, OUT_MAX), 0);
}

static void test_empty_preset_sends_nothing(void)
{
    setup();
    CHECK_EQ(button_engine_event(&eng, 2, BTN_EV_RELEASE | BTN_EV_TAP,
                                 out, OUT_MAX), 0);
}

static void test_preset_hold_requests_a_save(void)
{
    setup();
    int n = button_engine_event(&eng, 3, BTN_EV_HOLD, out, OUT_MAX);
    CHECK_EQ(n, BUTTON_ACTION_SAVE_PRESET);
    CHECK_EQ(button_engine_pending_save_slot(&eng), 1);
}

static void test_release_after_a_save_does_not_also_replay(void)
{
    setup();
    button_engine_event(&eng, 3, BTN_EV_HOLD, out, OUT_MAX);
    CHECK_EQ(button_engine_event(&eng, 3, BTN_EV_RELEASE, out, OUT_MAX), 0);
}

static void test_negative_preset_length_is_refused(void)
{
    setup();
    cc_msg_t scene[1] = { { 0, 102, 30 } };
    button_engine_set_preset(&eng, 0, scene, -1);
    const preset_slot_t *p = button_engine_get_preset(&eng, 0);
    CHECK_EQ(p->len, 0);
}

int main(void)
{
    RUN(test_freeze_press_turns_on);
    RUN(test_freeze_second_press_turns_off);
    RUN(test_freeze_ignores_release_entirely);
    RUN(test_freeze_survives_a_lost_release);
    RUN(test_shimmer_steps_and_wraps);
    RUN(test_shimmer_release_sends_nothing);
    RUN(test_cycle_starts_on_the_synths_default_step);
    RUN(test_recall_resyncs_the_cycle_step);
    RUN(test_sync_cycle_ignores_non_cycle_buttons);
    RUN(test_preset_tap_replays_the_stored_list);
    RUN(test_preset_press_alone_does_not_replay);
    RUN(test_empty_preset_sends_nothing);
    RUN(test_preset_hold_requests_a_save);
    RUN(test_release_after_a_save_does_not_also_replay);
    RUN(test_negative_preset_length_is_refused);
    TEST_MAIN_END();
}
