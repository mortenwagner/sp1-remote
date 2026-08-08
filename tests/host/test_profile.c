#include "profile.h"
#include "test_util.h"
#include <stdbool.h>

static void test_default_profile_matches_the_spec(void)
{
    const profile_t *p = &profile_popgoblin_default;

    CHECK_EQ(p->fader[0].cc, 102);
    CHECK_EQ(p->fader[1].cc, 104);
    CHECK_EQ(p->fader[2].cc, 107);
    CHECK_EQ(p->fader[3].cc, 108);

    for (int i = 0; i < PROFILE_NUM_FADERS; i++) {
        CHECK_EQ(p->fader[i].channel, 0);
    }

    CHECK_EQ(p->button[0].mode, BTN_MODE_TOGGLE);
    CHECK_EQ(p->button[0].cc,   64);
    CHECK_EQ(p->button[1].mode, BTN_MODE_CYCLE);
    CHECK_EQ(p->button[1].cc,   105);
    CHECK_EQ(p->button[1].n_steps, 4);
    CHECK_EQ(p->button[2].mode, BTN_MODE_PRESET);
    CHECK_EQ(p->button[2].preset_slot, 0);
    CHECK_EQ(p->button[3].mode, BTN_MODE_PRESET);
    CHECK_EQ(p->button[3].preset_slot, 1);
}

/* PopGoblin's ShimmerLevel iterates Boost, Full, Low, Off and set_from_cc
 * picks index = cc * 4 / 128. So the buckets are Boost 0-31, Full 32-63,
 * Low 64-95, Off 96-127, and the spec's off -> low -> full -> boost order
 * means DESCENDING CC values at the bucket centres. Getting this backwards
 * would silently invert the control. */
static void test_shimmer_steps_hit_the_right_enum_buckets(void)
{
    const button_cfg_t *b = &profile_popgoblin_default.button[1];
    CHECK_EQ(b->n_steps, 4);

    const char *name[4] = { "off", "low", "full", "boost" };
    const int   want[4] = { 3, 2, 1, 0 };   /* enum index the synth will pick */
    for (int i = 0; i < 4; i++) {
        int bucket = (b->steps[i] * 4) / 128;
        (void)name;
        CHECK_EQ(bucket, want[i]);
    }

    /* And the puck must start where the synth's default is: Full. */
    CHECK_EQ(b->init_step, 2);
    CHECK_EQ((b->steps[b->init_step] * 4) / 128, 1);   /* 1 == Full */
}

static void test_preset_capture_list_uses_existing_ccs_only(void)
{
    const profile_t *p = &profile_popgoblin_default;
    CHECK_EQ(p->preset_capture_len, 5);
    for (int i = 0; i < p->preset_capture_len; i++) {
        uint8_t cc = p->preset_capture[i];
        CHECK(cc != 64);
        bool known = false;
        for (int f = 0; f < PROFILE_NUM_FADERS; f++) {
            if (p->fader[f].cc == cc) known = true;
        }
        if (p->button[1].cc == cc) known = true;
        CHECK(known);
    }
}

int main(void)
{
    RUN(test_default_profile_matches_the_spec);
    RUN(test_shimmer_steps_hit_the_right_enum_buckets);
    RUN(test_preset_capture_list_uses_existing_ccs_only);
    TEST_MAIN_END();
}
