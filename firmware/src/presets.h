/* PURE. Preset records for the single 4 KB flash page at 0xFF000, which
 * the firmware flasher never erases (it stops at 0xFF000, see
 * solderless utility/js/protocol.js:10-11). One page is not enough for
 * Zephyr NVS, so the page is an append-log of fixed-size records: write
 * the next erased slot, read the last valid one, erase only when full. */
#ifndef SP1_PRESETS_H
#define SP1_PRESETS_H

#include <stdbool.h>
#include <stdint.h>
#include "buttons.h"

#define PRESET_REC_SIZE   40
#define PRESET_MAGIC_0    0x53   /* 'S' */
#define PRESET_MAGIC_1    0x50   /* 'P' */
#define PRESET_VERSION    1

typedef struct {
    preset_slot_t slot[BUTTON_MAX_PRESET_SLOTS];
} preset_bank_t;

/* Layout, 40 bytes:
 *   [0]      magic 'S'
 *   [1]      magic 'P'
 *   [2]      version
 *   [3]      reserved (0)
 *   [4]      slot 0 length
 *   [5..19]  slot 0, up to 5 entries of (channel, cc, value)
 *   [20]     slot 1 length
 *   [21..35] slot 1, up to 5 entries
 *   [36..38] reserved (0)
 *   [39]     CRC-8 over bytes 0 to 38 */
#define PRESET_ENTRIES 5

#if PROFILE_MAX_CAPTURE > PRESET_ENTRIES
#error "preset records cannot hold the profile's capture list"
#endif

void preset_record_encode(const preset_bank_t *bank, uint8_t rec[PRESET_REC_SIZE]);
bool preset_record_decode(const uint8_t rec[PRESET_REC_SIZE], preset_bank_t *out);

/* Index of the newest valid record, or -1 when the page holds none. */
int  preset_page_find_latest(const uint8_t *page, uint32_t page_len,
                             preset_bank_t *out);

/* Byte offset of the next writable slot, or -1 when the page is full. */
int  preset_page_next_offset(const uint8_t *page, uint32_t page_len);

/* ---- Zephyr-only half, implemented in presets_flash.c ----
 * Excluded from the host tests: everything above this line is pure. */
bool preset_store_is_safe(void);   /* is the page ours to write? */
bool preset_store_load(preset_bank_t *out);
bool preset_store_save(const preset_bank_t *bank);
void preset_store_dump(void);      /* Task 7.0, read only */
bool preset_store_repair(void);    /* erase the page; human-triggered only */

#endif /* SP1_PRESETS_H */
