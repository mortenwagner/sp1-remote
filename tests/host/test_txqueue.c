#include "txqueue.h"
#include "test_util.h"

static txqueue_t q;

static void test_fifo_order(void)
{
    txq_init(&q);
    CHECK(txq_push(&q, (cc_msg_t){ 0, 102, 10 }));
    CHECK(txq_push(&q, (cc_msg_t){ 0, 104, 20 }));
    CHECK_EQ(txq_count(&q), 2);

    cc_msg_t m;
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.cc, 102);
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.cc, 104);
    CHECK(!txq_pop(&q, &m));
}

/* The whole point: a fader swept faster than the wire drains must not
 * queue every intermediate value. The newest value replaces the pending
 * one and the queue depth stays at 1. */
static void test_same_cc_coalesces_to_the_latest_value(void)
{
    txq_init(&q);
    for (uint8_t v = 0; v < 100; v++) {
        CHECK(txq_push(&q, (cc_msg_t){ 0, 102, v }));
    }
    CHECK_EQ(txq_count(&q), 1);

    cc_msg_t m;
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.value, 99);
}

/* Coalescing must not reorder: a busy fader cannot push a waiting button
 * message to the back of the queue forever. */
static void test_coalescing_keeps_queue_position(void)
{
    txq_init(&q);
    txq_push(&q, (cc_msg_t){ 0, 102, 1 });
    txq_push(&q, (cc_msg_t){ 0,  64, 127 });
    txq_push(&q, (cc_msg_t){ 0, 102, 9 });
    CHECK_EQ(txq_count(&q), 2);

    cc_msg_t m;
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.cc, 102);
    CHECK_EQ(m.value, 9);
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.cc, 64);
}

static void test_same_cc_on_a_different_channel_is_a_different_message(void)
{
    txq_init(&q);
    txq_push(&q, (cc_msg_t){ 0, 102, 1 });
    txq_push(&q, (cc_msg_t){ 1, 102, 2 });
    CHECK_EQ(txq_count(&q), 2);
}

static void test_full_queue_rejects_new_but_still_coalesces(void)
{
    txq_init(&q);
    for (uint8_t i = 0; i < TXQ_MAX; i++) {
        CHECK(txq_push(&q, (cc_msg_t){ 0, (uint8_t)(1 + i), i }));
    }
    CHECK_EQ(txq_count(&q), TXQ_MAX);

    /* A brand new CC has nowhere to go. */
    CHECK(!txq_push(&q, (cc_msg_t){ 0, 99, 5 }));
    /* One already queued still updates in place. */
    CHECK(txq_push(&q, (cc_msg_t){ 0, 1, 77 }));
    CHECK_EQ(txq_count(&q), TXQ_MAX);

    cc_msg_t m;
    CHECK(txq_pop(&q, &m));
    CHECK_EQ(m.cc, 1);
    CHECK_EQ(m.value, 77);
}

static void test_wraps_around_the_ring(void)
{
    txq_init(&q);
    cc_msg_t m;
    for (int cycle = 0; cycle < 5; cycle++) {
        for (uint8_t i = 0; i < TXQ_MAX; i++) {
            CHECK(txq_push(&q, (cc_msg_t){ 0, (uint8_t)(1 + i), i }));
        }
        for (uint8_t i = 0; i < TXQ_MAX; i++) {
            CHECK(txq_pop(&q, &m));
            CHECK_EQ(m.cc, 1 + i);
        }
        CHECK_EQ(txq_count(&q), 0);
    }
}

int main(void)
{
    RUN(test_fifo_order);
    RUN(test_same_cc_coalesces_to_the_latest_value);
    RUN(test_coalescing_keeps_queue_position);
    RUN(test_same_cc_on_a_different_channel_is_a_different_message);
    RUN(test_full_queue_rejects_new_but_still_coalesces);
    RUN(test_wraps_around_the_ring);
    TEST_MAIN_END();
}
