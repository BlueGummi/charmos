#include "structures/tests/test_internal.h"

#ifdef TEST_SPSC_FIFO
TEST_GROUP_DECLARE(spsc_fifo);

TEST_DECLARE_UNIT(spsc_fifo, byte_stream_and_wraparound) {
    struct spsc_fifo fifo;
    bool ok = spsc_fifo_init(&fifo, 16);
    TEST_ASSERT(ok);
    TEST_ASSERT_EQ(fifo.size, 16);
    TEST_ASSERT(spsc_fifo_is_empty(&fifo));
    TEST_ASSERT_EQ(spsc_fifo_avail(&fifo), 16);

    const char *msg1 = "Hello, World!";
    size_t len1 = 13;
    size_t written = spsc_fifo_write(&fifo, msg1, len1);
    TEST_ASSERT_EQ(written, len1);
    TEST_ASSERT_EQ(spsc_fifo_len(&fifo), len1);
    TEST_ASSERT_EQ(spsc_fifo_avail(&fifo), 3);

    /* Peek without consuming */
    char peek_buf[16] = {0};
    size_t peeked = spsc_fifo_peek(&fifo, peek_buf, len1);
    TEST_ASSERT_EQ(peeked, len1);
    TEST_ASSERT(!memcmp(peek_buf, msg1, len1));
    TEST_ASSERT_EQ(spsc_fifo_len(&fifo), len1);

    /* Read partial */
    char read_buf[16] = {0};
    size_t read_bytes = spsc_fifo_read(&fifo, read_buf, 7);
    TEST_ASSERT_EQ(read_bytes, 7);
    TEST_ASSERT(!memcmp(read_buf, "Hello, ", 7));
    TEST_ASSERT_EQ(spsc_fifo_len(&fifo), 6);

    /* Write across ring wraparound boundary */
    const char *msg2 = "12345678";
    size_t written2 = spsc_fifo_write(&fifo, msg2, 8);
    TEST_ASSERT_EQ(written2, 8);
    TEST_ASSERT_EQ(spsc_fifo_len(&fifo), 14);

    /* Read the remainder of msg1 and all of msg2 */
    char full_read[16] = {0};
    size_t read_total = spsc_fifo_read(&fifo, full_read, 14);
    TEST_ASSERT_EQ(read_total, 14);
    TEST_ASSERT(!memcmp(full_read, "World!12345678", 14));
    TEST_ASSERT(spsc_fifo_is_empty(&fifo));

    spsc_fifo_destroy(&fifo);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(spsc_fifo, ptr_helpers) {
    struct spsc_fifo fifo;
    bool ok = spsc_fifo_init(&fifo, 8 * sizeof(void *));
    TEST_ASSERT(ok);

    void *ptrs[4] = {(void *) 0x1000, (void *) 0x2000, (void *) 0x3000,
                     (void *) 0x4000};
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT(spsc_fifo_push_ptr(&fifo, ptrs[i]));
    }

    for (int i = 0; i < 4; i++) {
        void *out = NULL;
        TEST_ASSERT(spsc_fifo_pop_ptr(&fifo, &out));
        TEST_ASSERT_EQ(out, ptrs[i]);
    }

    TEST_ASSERT(spsc_fifo_is_empty(&fifo));
    spsc_fifo_destroy(&fifo);
    return TEST_SUCCESS;
}
#endif
