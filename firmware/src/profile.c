#include "profile.h"

const profile_t profile_popgoblin_v2 = {
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

/* The PopGoblin loop engine (tiliqua docs/superpowers/specs/
 * 2026-11-06-popgoblin-loop-engine-design.md). Faders keep their CC
 * numbers; the synth reinterprets 107 as grains and 108 as loop keep.
 * PLAY (next chord) and the rocker (song) live in perform.c, outside this
 * four-button table. */
const profile_t profile_popgoblin_default = {
    .fader = {
        { .cc = 102, .channel = 0 },    /* tone  */
        { .cc = 104, .channel = 0 },    /* wash  */
        { .cc = 107, .channel = 0 },    /* grains */
        { .cc = 108, .channel = 0 },    /* loop keep */
    },
    .button = {
        {   /* freeze the loop */
            .mode = BTN_MODE_TOGGLE, .channel = 0, .cc = 64,
            .on_value = 127, .off_value = 0,
        },
        {   /* sub-drone on/off */
            .mode = BTN_MODE_TOGGLE, .channel = 0, .cc = 86,
            .on_value = 127, .off_value = 0,
        },
        { .mode = BTN_MODE_HOLD, .channel = 0, .cc = 87 },   /* stutter 1/8  */
        { .mode = BTN_MODE_HOLD, .channel = 0, .cc = 88 },   /* stutter 1/16 */
    },
    .preset_capture     = { 102, 104, 107, 108 },
    .preset_capture_len = 4,
};
