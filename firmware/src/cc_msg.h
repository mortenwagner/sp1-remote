/* PURE. One MIDI control-change message.
 *
 * Its own header, and not part of profile.h, purely for build ordering:
 * the transmit queue (Phase 3) needs this type but must not depend on the
 * profile table (Phase 4). Everything downstream gets it transitively. */
#ifndef SP1_CC_MSG_H
#define SP1_CC_MSG_H

#include <stdint.h>

typedef struct {
    uint8_t channel;
    uint8_t cc;
    uint8_t value;
} cc_msg_t;

/* A cc number no Control Change can have (CCs are 0..127). A message with
 * this cc is a Program Change whose program is `value`; the transmit layer
 * turns it into the right bytes. Kept in cc_msg_t so Program Change rides
 * the same coalescing queue as everything else. */
#define CC_MSG_PROGRAM 0x80u

#endif /* SP1_CC_MSG_H */
