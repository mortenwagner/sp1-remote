#include "profile.h"

const profile_t profile_popgoblin_default = {
    .fader = {
        { .cc = 102, .channel = 0 },
        { .cc = 104, .channel = 0 },
        { .cc = 107, .channel = 0 },
        { .cc = 108, .channel = 0 },
    },
    .button = {
        {
            .mode = BTN_MODE_TOGGLE, .channel = 0, .cc = 64,
            .on_value = 127, .off_value = 0,
        },
        {
            /* shimmer. PopGoblin's ShimmerLevel enum iterates
             * Boost, Full, Low, Off and set_from_cc picks
             * index = cc * 4 / 128, so the buckets are
             * Boost 0-31, Full 32-63, Low 64-95, Off 96-127.
             * Send bucket CENTRES, ordered off -> low -> full -> boost
             * per the spec. The synth's default is Full, which is why
             * init_step is 2: the puck starts in agreement without
             * transmitting anything. */
            .mode = BTN_MODE_CYCLE, .channel = 0, .cc = 105,
            .steps = { 112, 80, 48, 16 }, .n_steps = 4, .init_step = 2,
        },
        { .mode = BTN_MODE_PRESET, .channel = 0, .preset_slot = 0 },
        { .mode = BTN_MODE_PRESET, .channel = 0, .preset_slot = 1 },
    },
    .preset_capture     = { 102, 104, 107, 108, 105 },
    .preset_capture_len = 5,
};
