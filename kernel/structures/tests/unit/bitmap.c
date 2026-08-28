#include "structures/tests/test_internal.h"

#ifdef TEST_BITMAP
TEST_GROUP_DECLARE(bitmap, .intensity_desc = {
                               .curve = SCALE_PIECEWISE_LOG,
                               .unit = "bits",
                           });

/* Bitmaps are arrays of 64 bit words, so every op that takes a bit count
 * has a partial trailing word to deal with, which can cause counting/reporting
 * bits that live past nbits inside the last word, and mishandling of cases
 * where nbits lands on a word boundary
 *
 * We try to test exact boundaries and off-by-ones, 64, 1, 63, 65, 128, etc */

#define BM_WORDS 4
#define BM_BITS (BM_WORDS * BITMAP_BITS_PER_WORD)

static void bm_reset(bitmap_word_t *map) {
    for (size_t i = 0; i < BM_WORDS; i++)
        map[i] = 0;
}

static void bm_fill(bitmap_word_t *map) {
    for (size_t i = 0; i < BM_WORDS; i++)
        map[i] = ~(bitmap_word_t) 0;
}

static void bm_reset_n(bitmap_word_t *map, size_t nwords) {
    for (size_t i = 0; i < nwords; i++)
        map[i] = 0;
}

static void bm_fill_n(bitmap_word_t *map, size_t nwords) {
    for (size_t i = 0; i < nwords; i++)
        map[i] = ~(bitmap_word_t) 0;
}

TEST_DECLARE_UNIT(bitmap, set_test_clear, TEST_INTENSITY(64, 256, 4096)) {
    size_t nbits = ctx->intensity_val ? ctx->intensity_val : BM_BITS;
    size_t nwords = BITMAP_WORDS(nbits);
    bitmap_word_t *map =
        kmalloc(sizeof(bitmap_word_t) * nwords, ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(map);

    for (size_t bit = 0; bit < nbits; bit++) {
        TEST_ASSERT(!bitmap_test(map, bit));
        bitmap_set(map, bit);
        TEST_ASSERT(bitmap_test(map, bit));

        /* Setting one bit must not
         * disturb neighbors across the word boundary */
        if (bit > 0)
            TEST_ASSERT(!bitmap_test(map, bit - 1));

        bitmap_clear(map, bit);
        TEST_ASSERT(!bitmap_test(map, bit));
    }

    kfree(map);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(bitmap, bit_isolation, TEST_INTENSITY(64, 256, 1024)) {
    size_t nbits = ctx->intensity_val ? ctx->intensity_val : BM_BITS;
    size_t nwords = BITMAP_WORDS(nbits);
    bitmap_word_t *map =
        kmalloc(sizeof(bitmap_word_t) * nwords, ALLOC_FLAGS_NONE);
    TEST_ASSERT_NONNULL(map);

    for (size_t bit = 0; bit < nbits; bit++) {
        bm_reset_n(map, nwords);
        bitmap_set(map, bit);

        for (size_t other = 0; other < nbits; other++)
            TEST_ASSERT_EQ(bitmap_test(map, other), (other == bit));

        TEST_ASSERT_EQ(bitmap_weight(map, nbits), 1);
    }

    kfree(map);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(bitmap, toggle_and_test_ops) {
    bitmap_word_t map[BM_WORDS];
    bm_reset(map);

    bitmap_toggle(map, 70);
    TEST_ASSERT(bitmap_test(map, 70));
    bitmap_toggle(map, 70);
    TEST_ASSERT(!bitmap_test(map, 70));

    /* The test_and_* must report the state from before change */
    TEST_ASSERT_EQ(bitmap_test_and_set(map, 5), false);
    TEST_ASSERT_EQ(bitmap_test_and_set(map, 5), true);
    TEST_ASSERT_EQ(bitmap_test_and_clear(map, 5), true);
    TEST_ASSERT_EQ(bitmap_test_and_clear(map, 5), false);

    return TEST_SUCCESS;
}

/* Bits above nbits are memory and could be set, but shan't be counted */
TEST_DECLARE_UNIT(bitmap, weight_ignores_past_end) {
    bitmap_word_t map[BM_WORDS];
    bm_fill(map);

    static const size_t sizes[] = {1, 7, 63, 64, 65, 100, 127, 128, BM_BITS};

    for (size_t i = 0; i < TEST_ARRAY_LEN(sizes); i++)
        TEST_ASSERT_EQ(bitmap_weight(map, sizes[i]), sizes[i]);

    bm_reset(map);
    TEST_ASSERT_EQ(bitmap_weight(map, BM_BITS), 0);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(bitmap, weight_counts, TEST_INTENSITY(64, 256, 4096)) {
    size_t nbits = ctx->intensity_val ? ctx->intensity_val : BM_BITS;
    size_t nwords = BITMAP_WORDS(nbits);
    bitmap_word_t *map =
        kmalloc(sizeof(bitmap_word_t) * nwords, ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(map);

    for (size_t n = 0; n < nbits; n++) {
        TEST_ASSERT_EQ(bitmap_weight(map, nbits), n);
        bitmap_set(map, n);
    }
    TEST_ASSERT_EQ(bitmap_weight(map, nbits), nbits);

    kfree(map);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(bitmap, ranges) {
    bitmap_word_t map[BM_WORDS];
    bm_reset(map);

    /* range over word boundary */
    bitmap_set_range(map, 60, 10);
    for (size_t bit = 0; bit < BM_BITS; bit++)
        TEST_ASSERT_EQ(bitmap_test(map, bit), (bit >= 60 && bit < 70));

    bitmap_clear_range(map, 62, 4);
    for (size_t bit = 60; bit < 70; bit++)
        TEST_ASSERT_EQ(bitmap_test(map, bit), (bit < 62 || bit >= 66));

    /* zero length range == no-op */
    bm_reset(map);
    bitmap_set_range(map, 10, 0);
    TEST_ASSERT_EQ(bitmap_weight(map, BM_BITS), 0);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(bitmap, find_first_set, TEST_INTENSITY(64, 256, 2048)) {
    size_t nbits = ctx->intensity_val ? ctx->intensity_val : BM_BITS;
    size_t nwords = BITMAP_WORDS(nbits);
    bitmap_word_t *map =
        kmalloc(sizeof(bitmap_word_t) * nwords, ALLOC_FLAGS_NONE);
    TEST_ASSERT_NONNULL(map);

    bm_reset_n(map, nwords);
    TEST_ASSERT_EQ(bitmap_find_first_set(map, nbits), nbits);

    for (size_t bit = 0; bit < nbits; bit++) {
        bm_reset_n(map, nwords);
        bitmap_set(map, bit);
        TEST_ASSERT_EQ(bitmap_find_first_set(map, nbits), bit);
    }

    /* Bit set beyond nbits mustn't report as found */
    if (nbits >= 128) {
        bm_reset_n(map, nwords);
        bitmap_set(map, 100);
        TEST_ASSERT_EQ(bitmap_find_first_set(map, 100), 100);
        TEST_ASSERT_EQ(bitmap_find_first_set(map, 101), 100);
    }

    kfree(map);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(bitmap, find_first_zero, TEST_INTENSITY(64, 256, 2048)) {
    size_t nbits = ctx->intensity_val ? ctx->intensity_val : BM_BITS;
    size_t nwords = BITMAP_WORDS(nbits);
    bitmap_word_t *map =
        kmalloc(sizeof(bitmap_word_t) * nwords, ALLOC_FLAGS_NONE);
    TEST_ASSERT_NONNULL(map);

    bm_reset_n(map, nwords);
    TEST_ASSERT_EQ(bitmap_find_first_zero(map, nbits), 0);

    bm_fill_n(map, nwords);
    TEST_ASSERT_EQ(bitmap_find_first_zero(map, nbits), nbits);

    for (size_t bit = 0; bit < nbits; bit++) {
        bm_fill_n(map, nwords);
        bitmap_clear(map, bit);
        TEST_ASSERT_EQ(bitmap_find_first_zero(map, nbits), bit);
    }

    /* Full at word boundary must report nothing free */
    if (nbits >= 128) {
        bm_fill_n(map, nwords);
        TEST_ASSERT_EQ(bitmap_find_first_zero(map, 64), 64);
        TEST_ASSERT_EQ(bitmap_find_first_zero(map, 128), 128);
    }

    kfree(map);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(bitmap, find_next_bit) {
    bitmap_word_t map[BM_WORDS];
    bm_reset(map);

    bitmap_set(map, 3);
    bitmap_set(map, 64);
    bitmap_set(map, 65);
    bitmap_set(map, 200);

    /* Starting on a set bit finds that bit */
    TEST_ASSERT_EQ(bitmap_find_next_bit(map, BM_BITS, 0), 3);
    TEST_ASSERT_EQ(bitmap_find_next_bit(map, BM_BITS, 3), 3);
    TEST_ASSERT_EQ(bitmap_find_next_bit(map, BM_BITS, 4), 64);
    TEST_ASSERT_EQ(bitmap_find_next_bit(map, BM_BITS, 64), 64);
    TEST_ASSERT_EQ(bitmap_find_next_bit(map, BM_BITS, 65), 65);
    TEST_ASSERT_EQ(bitmap_find_next_bit(map, BM_BITS, 66), 200);
    TEST_ASSERT_EQ(bitmap_find_next_bit(map, BM_BITS, 201), BM_BITS);

    /* A start at or past the end isn't a search */
    TEST_ASSERT_EQ(bitmap_find_next_bit(map, BM_BITS, BM_BITS), BM_BITS);
    TEST_ASSERT_EQ(bitmap_find_next_bit(map, BM_BITS, BM_BITS + 10), BM_BITS);

    /* Bits past nbits stay invisible */
    TEST_ASSERT_EQ(bitmap_find_next_bit(map, 100, 66), 100);

    return TEST_SUCCESS;
}

/* Walking every set bit via find_next_bit must visit the bits that
 * bitmap_test agrees are set, and terminate */
TEST_DECLARE_UNIT(bitmap, find_next_bit_walk, TEST_INTENSITY(64, 256, 4096)) {
    size_t nbits = ctx->intensity_val ? ctx->intensity_val : BM_BITS;
    size_t nwords = BITMAP_WORDS(nbits);
    bitmap_word_t *map =
        kmalloc(sizeof(bitmap_word_t) * nwords, ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(map);

    for (size_t bit = 0; bit < nbits; bit += 7)
        bitmap_set(map, bit);

    size_t seen = 0;
    for (size_t bit = bitmap_find_next_bit(map, nbits, 0); bit < nbits;
         bit = bitmap_find_next_bit(map, nbits, bit + 1)) {
        TEST_ASSERT(bitmap_test(map, bit));
        TEST_ASSERT_EQ(bit % 7, 0);
        seen++;
    }

    TEST_ASSERT_EQ(seen, bitmap_weight(map, nbits));

    kfree(map);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(bitmap, word_index_math) {
    TEST_ASSERT_EQ(BITMAP_WORD_INDEX(0), 0);
    TEST_ASSERT_EQ(BITMAP_WORD_INDEX(63), 0);
    TEST_ASSERT_EQ(BITMAP_WORD_INDEX(64), 1);
    TEST_ASSERT_EQ(BITMAP_BIT_OFFSET(64), 0);
    TEST_ASSERT_EQ(BITMAP_BIT_OFFSET(65), 1);

    /* BITMAP_WORDS rounds up */
    TEST_ASSERT_EQ(BITMAP_WORDS(1), 1);
    TEST_ASSERT_EQ(BITMAP_WORDS(64), 1);
    TEST_ASSERT_EQ(BITMAP_WORDS(65), 2);
    TEST_ASSERT_EQ(BITMAP_WORDS(128), 2);

    return TEST_SUCCESS;
}

#endif
