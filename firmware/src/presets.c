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
