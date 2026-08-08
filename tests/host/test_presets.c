#include <string.h>
#include "presets.h"
#include "test_util.h"

#define PAGE_LEN 4096

static uint8_t page[PAGE_LEN];

static void erase_page(void) { memset(page, 0xFF, sizeof(page)); }

static preset_bank_t make_bank(uint8_t seed)
{
    preset_bank_t b;
    memset(&b, 0, sizeof(b));
    b.slot[0].len = 2;
    b.slot[0].msg[0] = (cc_msg_t){ 0, 102, (uint8_t)(seed + 1) };
    b.slot[0].msg[1] = (cc_msg_t){ 0, 104, (uint8_t)(seed + 2) };
    b.slot[1].len = 1;
    b.slot[1].msg[0] = (cc_msg_t){ 0, 107, (uint8_t)(seed + 3) };
    return b;
}

static void test_round_trip(void)
{
    uint8_t rec[PRESET_REC_SIZE];
    preset_bank_t in = make_bank(10), out;
    preset_record_encode(&in, rec);
    CHECK(preset_record_decode(rec, &out));
    CHECK_EQ(out.slot[0].len, 2);
    CHECK_EQ(out.slot[0].msg[1].cc, 104);
    CHECK_EQ(out.slot[0].msg[1].value, 12);
    CHECK_EQ(out.slot[1].msg[0].value, 13);
}

static void test_corrupt_record_is_rejected(void)
{
    uint8_t rec[PRESET_REC_SIZE];
    preset_bank_t in = make_bank(10), out;
    preset_record_encode(&in, rec);
    rec[5] ^= 0xFF;                       /* flip a payload bit */
    CHECK(!preset_record_decode(rec, &out));
}

static void test_erased_record_is_rejected(void)
{
    uint8_t rec[PRESET_REC_SIZE];
    preset_bank_t out;
    memset(rec, 0xFF, sizeof(rec));
    CHECK(!preset_record_decode(rec, &out));
}

static void test_empty_page_has_no_latest_and_starts_at_zero(void)
{
    erase_page();
    preset_bank_t out;
    CHECK_EQ(preset_page_find_latest(page, PAGE_LEN, &out), -1);
    CHECK_EQ(preset_page_next_offset(page, PAGE_LEN), 0);
}

static void test_latest_wins(void)
{
    erase_page();
    preset_bank_t a = make_bank(1), b = make_bank(50), out;
    preset_record_encode(&a, page + 0);
    preset_record_encode(&b, page + PRESET_REC_SIZE);
    CHECK_EQ(preset_page_find_latest(page, PAGE_LEN, &out), 1);
    CHECK_EQ(out.slot[0].msg[0].value, 51);
    CHECK_EQ(preset_page_next_offset(page, PAGE_LEN), 2 * PRESET_REC_SIZE);
}

static void test_corrupt_tail_falls_back_to_the_last_good_record(void)
{
    erase_page();
    preset_bank_t a = make_bank(1), out;
    preset_record_encode(&a, page + 0);
    preset_record_encode(&a, page + PRESET_REC_SIZE);
    page[PRESET_REC_SIZE + 4] ^= 0xFF;    /* the newer record is damaged */
    CHECK_EQ(preset_page_find_latest(page, PAGE_LEN, &out), 0);
    CHECK_EQ(out.slot[0].msg[0].value, 2);
}

static void test_full_page_reports_no_room(void)
{
    erase_page();
    preset_bank_t a = make_bank(1);
    for (uint32_t off = 0; off + PRESET_REC_SIZE <= PAGE_LEN;
         off += PRESET_REC_SIZE) {
        preset_record_encode(&a, page + off);
    }
    CHECK_EQ(preset_page_next_offset(page, PAGE_LEN), -1);
}

int main(void)
{
    RUN(test_round_trip);
    RUN(test_corrupt_record_is_rejected);
    RUN(test_erased_record_is_rejected);
    RUN(test_empty_page_has_no_latest_and_starts_at_zero);
    RUN(test_latest_wins);
    RUN(test_corrupt_tail_falls_back_to_the_last_good_record);
    RUN(test_full_page_reports_no_room);
    TEST_MAIN_END();
}
