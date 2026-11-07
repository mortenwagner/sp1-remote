/* PURE. The performance layer for the PopGoblin loop engine
 * (tiliqua docs/superpowers/specs/2026-11-06-popgoblin-loop-engine-design.md).
 *
 *   rocker      Program Change 0..7: the song, with wrap
 *   PLAY        CC 85 = 127: next chord (the Tiliqua lands it on the beat)
 *   track 3, 4  CC 87 / 88 while held: 127 on the press, repeated every
 *               100 ms, 0 on the release. The Tiliqua drops a stutter
 *               after 300 ms of silence, so a lost release costs a
 *               fraction of a second instead of a stuck stutter.
 *
 * The side LEDs show the song number in binary, like Syrup's presets. The
 * puck cannot show the chord position: MIDI is one-way and the song's
 * chord list lives on the Tiliqua. A PLAY press flashes the column. */
#ifndef SP1_PERFORM_H
#define SP1_PERFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "cc_msg.h"

#define PERFORM_NUM_SONGS      8
#define PERFORM_CC_NEXT_CHORD  85
#define PERFORM_CC_STUTTER_8   87
#define PERFORM_CC_STUTTER_16  88
#define PERFORM_HOLD_REPEAT_MS 100
#define PERFORM_FLASH_MS       120
#define PERFORM_NUM_HOLDS      2

typedef struct {
    uint8_t  channel;
    uint8_t  song;
    bool     held[PERFORM_NUM_HOLDS];
    uint32_t last_ms[PERFORM_NUM_HOLDS];
    uint32_t flash_until;
} perform_t;

void perform_init(perform_t *p, uint8_t channel);

/* dir = -1 or +1. Writes one Program Change (CC_MSG_PROGRAM). */
int  perform_rocker(perform_t *p, int dir, cc_msg_t *out);

/* On the PLAY press. */
int  perform_play(perform_t *p, uint32_t now_ms, cc_msg_t *out);

/* Call every control pass with the button's current state. Returns the
 * number of messages written (0 or 1). */
int  perform_hold(perform_t *p, int which, bool down, uint32_t now_ms,
                  cc_msg_t *out);

bool    perform_is_held(const perform_t *p, int which);
uint8_t perform_side_code(const perform_t *p);
bool    perform_flash(const perform_t *p, uint32_t now_ms);

#endif /* SP1_PERFORM_H */
