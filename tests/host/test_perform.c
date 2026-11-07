/* The performance layer: rocker -> song (Program Change), PLAY -> next
 * chord, held stutters with repeats, and the side-LED song code. */
#include "perform.h"
#include "test_util.h"

static perform_t p;
static cc_msg_t out[4];

static void test_rocker_steps_songs_with_wrap(void)
{
    perform_init(&p, 0);
    CHECK_EQ(perform_rocker(&p, +1, out), 1);
    CHECK_EQ(out[0].cc, CC_MSG_PROGRAM);
    CHECK_EQ(out[0].value, 1);
    perform_rocker(&p, -1, out);
    CHECK_EQ(out[0].value, 0);
    perform_rocker(&p, -1, out);
    CHECK_EQ(out[0].value, PERFORM_NUM_SONGS - 1);   /* wraps down */
    perform_rocker(&p, +1, out);
    CHECK_EQ(out[0].value, 0);                       /* and up */
}

static void test_play_asks_for_the_next_chord(void)
{
    perform_init(&p, 0);
    CHECK_EQ(perform_play(&p, 1000, out), 1);
    CHECK_EQ(out[0].cc, PERFORM_CC_NEXT_CHORD);
    CHECK_EQ(out[0].value, 127);
    CHECK(perform_flash(&p, 1050));
    CHECK(!perform_flash(&p, 1000 + PERFORM_FLASH_MS));
}

/* While held: 127 on the press, then again every 100 ms, so the Tiliqua's
 * 300 ms timeout never fires; 0 on the release. */
static void test_hold_repeats_then_releases(void)
{
    perform_init(&p, 0);
    CHECK_EQ(perform_hold(&p, 0, true, 0, out), 1);
    CHECK_EQ(out[0].cc, PERFORM_CC_STUTTER_8);
    CHECK_EQ(out[0].value, 127);
    CHECK_EQ(perform_hold(&p, 0, true, 50, out), 0);
    CHECK_EQ(perform_hold(&p, 0, true, 100, out), 1);
    CHECK_EQ(out[0].value, 127);
    CHECK_EQ(perform_hold(&p, 0, true, 150, out), 0);
    CHECK(perform_is_held(&p, 0));
    CHECK_EQ(perform_hold(&p, 0, false, 160, out), 1);
    CHECK_EQ(out[0].value, 0);
    CHECK(!perform_is_held(&p, 0));
    CHECK_EQ(perform_hold(&p, 0, false, 300, out), 0);   /* quiet when up */
}

static void test_second_stutter_uses_its_own_cc(void)
{
    perform_init(&p, 0);
    perform_hold(&p, 1, true, 0, out);
    CHECK_EQ(out[0].cc, PERFORM_CC_STUTTER_16);
}

static void test_side_code_is_song_number(void)
{
    perform_init(&p, 0);
    CHECK_EQ(perform_side_code(&p), 1);
    perform_rocker(&p, +1, out);
    perform_rocker(&p, +1, out);
    CHECK_EQ(perform_side_code(&p), 3);    /* song 3 = LEDs 1 and 2 */
}

int main(void)
{
    RUN(test_rocker_steps_songs_with_wrap);
    RUN(test_play_asks_for_the_next_chord);
    RUN(test_hold_repeats_then_releases);
    RUN(test_second_stutter_uses_its_own_cc);
    RUN(test_side_code_is_song_number);
    TEST_MAIN_END();
}
