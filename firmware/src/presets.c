#include <string.h>
#include "presets.h"

static uint8_t crc8(const uint8_t *p, uint32_t len)
{
    uint8_t crc = 0x00;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (uint8_t)((crc & 0x80u) ? ((crc << 1) ^ 0x07u) : (crc << 1));
        }
    }
    return crc;
}

void preset_record_encode(const preset_bank_t *bank, uint8_t rec[PRESET_REC_SIZE])
{
    memset(rec, 0, PRESET_REC_SIZE);
    rec[0] = PRESET_MAGIC_0;
    rec[1] = PRESET_MAGIC_1;
    rec[2] = PRESET_VERSION;

    uint32_t o = 4;
    for (int s = 0; s < BUTTON_MAX_PRESET_SLOTS; s++) {
        uint8_t len = bank->slot[s].len;
        if (len > PRESET_ENTRIES) {
            len = PRESET_ENTRIES;
        }
        rec[o++] = len;
        for (int i = 0; i < PRESET_ENTRIES; i++) {
            if (i < len) {
                rec[o++] = bank->slot[s].msg[i].channel;
                rec[o++] = bank->slot[s].msg[i].cc;
                rec[o++] = bank->slot[s].msg[i].value;
            } else {
                o += 3;
            }
        }
    }
    rec[PRESET_REC_SIZE - 1] = crc8(rec, PRESET_REC_SIZE - 1);
}

bool preset_record_decode(const uint8_t rec[PRESET_REC_SIZE], preset_bank_t *out)
{
    if (rec[0] != PRESET_MAGIC_0 || rec[1] != PRESET_MAGIC_1 ||
        rec[2] != PRESET_VERSION) {
        return false;
    }
    if (crc8(rec, PRESET_REC_SIZE - 1) != rec[PRESET_REC_SIZE - 1]) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    uint32_t o = 4;
    for (int s = 0; s < BUTTON_MAX_PRESET_SLOTS; s++) {
        uint8_t len = rec[o++];
        if (len > PRESET_ENTRIES) {
            return false;
        }
        out->slot[s].len = len;
        for (int i = 0; i < PRESET_ENTRIES; i++) {
            if (i < len) {
                out->slot[s].msg[i].channel = rec[o];
                out->slot[s].msg[i].cc      = rec[o + 1];
                out->slot[s].msg[i].value   = rec[o + 2];
            }
            o += 3;
        }
    }
    return true;
}

int preset_page_find_latest(const uint8_t *page, uint32_t page_len,
                            preset_bank_t *out)
{
    int found = -1;
    for (uint32_t off = 0, idx = 0; off + PRESET_REC_SIZE <= page_len;
         off += PRESET_REC_SIZE, idx++) {
        preset_bank_t tmp;
        if (preset_record_decode(page + off, &tmp)) {
            *out  = tmp;
            found = (int)idx;
        }
    }
    return found;
}

int preset_page_next_offset(const uint8_t *page, uint32_t page_len)
{
    for (uint32_t off = 0; off + PRESET_REC_SIZE <= page_len;
         off += PRESET_REC_SIZE) {
        bool erased = true;
        for (uint32_t i = 0; i < PRESET_REC_SIZE; i++) {
            if (page[off + i] != 0xFF) { erased = false; break; }
        }
        if (erased) {
            return (int)off;
        }
    }
    return -1;
}

/* ======================================================================
 *  Profile records
 *
 *  Layout, 144 bytes:
 *    [0]       'S'
 *    [1]       'C'
 *    [2]       version
 *    [3]       reserved
 *    [4..11]   4 faders x (cc, channel)
 *    [12..139] 4 buttons x 32 bytes
 *    [140..142] preset_capture[0..2] is NOT stored: the capture list is a
 *              property of the build, not of the user's mapping
 *    [143]     CRC-8 over 0..142
 *
 *  A button is 32 bytes: mode, channel, cc, on_value, off_value,
 *  steps[4], n_steps, init_step, preset_slot, n_list, then 5 list entries
 *  of (channel, cc, value) = 15, then 3 reserved.
 * ====================================================================== */
#define PROF_BTN_SIZE 32

static void put_button(uint8_t *d, const button_cfg_t *b)
{
    memset(d, 0, PROF_BTN_SIZE);
    d[0] = (uint8_t)b->mode;
    d[1] = b->channel;
    d[2] = b->cc;
    d[3] = b->on_value;
    d[4] = b->off_value;
    for (int i = 0; i < PROFILE_MAX_STEPS; i++) {
        d[5 + i] = b->steps[i];
    }
    d[9]  = b->n_steps;
    d[10] = b->init_step;
    d[11] = b->preset_slot;
    d[12] = b->n_list;
    for (int i = 0; i < PROFILE_MAX_BTN_LIST; i++) {
        d[13 + i * 3 + 0] = b->list[i].channel;
        d[13 + i * 3 + 1] = b->list[i].cc;
        d[13 + i * 3 + 2] = b->list[i].value;
    }
}

static bool get_button(const uint8_t *d, button_cfg_t *b)
{
    if (d[0] > (uint8_t)BTN_MODE_LIST) {
        return false;
    }
    memset(b, 0, sizeof(*b));
    b->mode      = (btn_mode_t)d[0];
    b->channel   = d[1];
    b->cc        = d[2];
    b->on_value  = d[3];
    b->off_value = d[4];
    for (int i = 0; i < PROFILE_MAX_STEPS; i++) {
        b->steps[i] = d[5 + i];
    }
    b->n_steps    = d[9]  > PROFILE_MAX_STEPS    ? PROFILE_MAX_STEPS    : d[9];
    b->init_step  = d[10];
    b->preset_slot = d[11];
    b->n_list     = d[12] > PROFILE_MAX_BTN_LIST ? PROFILE_MAX_BTN_LIST : d[12];
    for (int i = 0; i < PROFILE_MAX_BTN_LIST; i++) {
        b->list[i].channel = d[13 + i * 3 + 0];
        b->list[i].cc      = d[13 + i * 3 + 1];
        b->list[i].value   = d[13 + i * 3 + 2];
    }
    return true;
}

void profile_record_encode(const profile_t *prof, uint8_t rec[PROFILE_REC_SIZE])
{
    memset(rec, 0, PROFILE_REC_SIZE);
    rec[0] = PROFILE_MAGIC_0;
    rec[1] = PROFILE_MAGIC_1;
    rec[2] = PROFILE_REC_VER;

    for (int f = 0; f < PROFILE_NUM_FADERS; f++) {
        rec[4 + f * 2 + 0] = prof->fader[f].cc;
        rec[4 + f * 2 + 1] = prof->fader[f].channel;
    }
    for (int b = 0; b < PROFILE_NUM_BUTTONS; b++) {
        put_button(&rec[12 + b * PROF_BTN_SIZE], &prof->button[b]);
    }
    rec[PROFILE_REC_SIZE - 1] = crc8(rec, PROFILE_REC_SIZE - 1);
}

/* The capture list is NOT restored from flash: it belongs to the build, and
 * a stored profile from an older build must not shrink it. Callers seed
 * *out from the compiled default before decoding. */
bool profile_record_decode(const uint8_t rec[PROFILE_REC_SIZE], profile_t *out)
{
    if (rec[0] != PROFILE_MAGIC_0 || rec[1] != PROFILE_MAGIC_1 ||
        rec[2] != PROFILE_REC_VER) {
        return false;
    }
    if (crc8(rec, PROFILE_REC_SIZE - 1) != rec[PROFILE_REC_SIZE - 1]) {
        return false;
    }
    for (int f = 0; f < PROFILE_NUM_FADERS; f++) {
        out->fader[f].cc      = rec[4 + f * 2 + 0] & 0x7Fu;
        out->fader[f].channel = rec[4 + f * 2 + 1] & 0x0Fu;
    }
    for (int b = 0; b < PROFILE_NUM_BUTTONS; b++) {
        if (!get_button(&rec[12 + b * PROF_BTN_SIZE], &out->button[b])) {
            return false;
        }
    }
    return true;
}

int profile_page_find_latest(const uint8_t *page, uint32_t page_len,
                             profile_t *out)
{
    int found = -1;

    for (uint32_t off = 0, idx = 0; off + PROFILE_REC_SIZE <= page_len;
         off += PROFILE_REC_SIZE, idx++) {
        profile_t tmp = *out;          /* seed, so the capture list survives */

        if (profile_record_decode(page + off, &tmp)) {
            *out  = tmp;
            found = (int)idx;
        }
    }
    return found;
}

int profile_page_next_offset(const uint8_t *page, uint32_t page_len)
{
    for (uint32_t off = 0; off + PROFILE_REC_SIZE <= page_len;
         off += PROFILE_REC_SIZE) {
        bool erased = true;

        for (uint32_t i = 0; i < PROFILE_REC_SIZE; i++) {
            if (page[off + i] != 0xFF) { erased = false; break; }
        }
        if (erased) {
            return (int)off;
        }
    }
    return -1;
}
