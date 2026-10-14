/* @title: Bitmap */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <math/align.h>
#include <math/bit.h>
#include <math/min_max.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t bitmap_word_t;

#define BITMAP_BITS_PER_WORD 64
#define BITMAP_WORDS(nbits)                                                    \
    ({                                                                         \
        __auto_type __nbits = (nbits);                                         \
        ct_typecheck_widenable_to((size_t) 0, nbits);                          \
        DIV_ROUND_UP((size_t) __nbits, BITMAP_BITS_PER_WORD);                  \
    })

/* Integer constant expression variant */
#define BITMAP_WORDS_CONST(nbits)                                              \
    (((nbits) / BITMAP_BITS_PER_WORD) + !!((nbits) % BITMAP_BITS_PER_WORD))
#define BITMAP_DECLARE(name, nbits)                                            \
    bitmap_word_t name[BITMAP_WORDS_CONST(nbits)]

#define BITMAP_WORD_INDEX(bit) ((bit) / BITMAP_BITS_PER_WORD)
#define BITMAP_BIT_OFFSET(bit) ((bit) % BITMAP_BITS_PER_WORD)
#define BITMAP_BIT_MASK(bit) ((bitmap_word_t) BIT(BITMAP_BIT_OFFSET(bit)))

#define bitmap_for_each_bit_set(bit, map, nbits)                               \
    for (size_t bit = 0; bit < (nbits); bit++)                                 \
        if (bitmap_test((map), (bit)))

#define bitmap_for_each_bit_unset(bit, map, nbits)                             \
    for (size_t bit = 0; bit < (nbits); bit++)                                 \
        if (!bitmap_test((map), (bit)))

static inline void bitmap_set(bitmap_word_t *map, size_t bit) {
    map[BITMAP_WORD_INDEX(bit)] |= BITMAP_BIT_MASK(bit);
}

static inline void bitmap_clear(bitmap_word_t *map, size_t bit) {
    map[BITMAP_WORD_INDEX(bit)] &= ~BITMAP_BIT_MASK(bit);
}

static inline void bitmap_toggle(bitmap_word_t *map, size_t bit) {
    map[BITMAP_WORD_INDEX(bit)] ^= BITMAP_BIT_MASK(bit);
}

static inline bool bitmap_test(const bitmap_word_t *map, size_t bit) {
    return (map[BITMAP_WORD_INDEX(bit)] & BITMAP_BIT_MASK(bit)) != 0;
}

static inline bool bitmap_test_and_set(bitmap_word_t *map, size_t bit) {
    bool old = bitmap_test(map, bit);
    bitmap_set(map, bit);
    return old;
}

static inline bool bitmap_test_and_clear(bitmap_word_t *map, size_t bit) {
    bool old = bitmap_test(map, bit);
    bitmap_clear(map, bit);
    return old;
}

static inline void bitmap_atomic_set(bitmap_word_t *map, size_t bit) {
    atomic(bitmap_word_t) *atom =
        (atomic(bitmap_word_t) *) &map[BITMAP_WORD_INDEX(bit)];
    atomic_set_bit_rlx(atom, BITMAP_BIT_OFFSET(bit));
}

static inline void bitmap_atomic_clear(bitmap_word_t *map, size_t bit) {
    atomic(bitmap_word_t) *atom =
        (atomic(bitmap_word_t) *) &map[BITMAP_WORD_INDEX(bit)];
    atomic_clear_bit_rlx(atom, BITMAP_BIT_OFFSET(bit));
}

static inline void bitmap_atomic_toggle(bitmap_word_t *map, size_t bit) {
    atomic(bitmap_word_t) *atom =
        (atomic(bitmap_word_t) *) &map[BITMAP_WORD_INDEX(bit)];
    atomic_toggle_bit_rlx(atom, BITMAP_BIT_OFFSET(bit));
}

static inline bool bitmap_atomic_test(const bitmap_word_t *map, size_t bit) {
    const atomic(bitmap_word_t) *atom =
        (const atomic(bitmap_word_t) *) &map[BITMAP_WORD_INDEX(bit)];
    return atomic_test_bit_rlx(atom, BITMAP_BIT_OFFSET(bit));
}

static inline bool bitmap_atomic_test_and_set(bitmap_word_t *map, size_t bit) {
    atomic(bitmap_word_t) *atom =
        (atomic(bitmap_word_t) *) &map[BITMAP_WORD_INDEX(bit)];
    return atomic_test_and_set_bit_acq_rel(atom, BITMAP_BIT_OFFSET(bit));
}

static inline bool bitmap_atomic_test_and_clear(bitmap_word_t *map,
                                                size_t bit) {
    atomic(bitmap_word_t) *atom =
        (atomic(bitmap_word_t) *) &map[BITMAP_WORD_INDEX(bit)];
    return atomic_test_and_clear_bit_acq_rel(atom, BITMAP_BIT_OFFSET(bit));
}

static inline void bitmap_zero(bitmap_word_t *map, size_t nbits) {
    memset(map, 0, BITMAP_WORDS(nbits) * sizeof(bitmap_word_t));
}

static inline void bitmap_copy(bitmap_word_t *dst, const bitmap_word_t *src,
                               size_t nbits) {
    memcpy(dst, src, BITMAP_WORDS(nbits) * sizeof(bitmap_word_t));
}

static inline void bitmap_fill(bitmap_word_t *map, size_t nbits) {
    for (size_t i = 0; i * BITMAP_BITS_PER_WORD < nbits; i++) {
        size_t left = nbits - i * BITMAP_BITS_PER_WORD;
        map[i] = left >= BITMAP_BITS_PER_WORD ? ~(bitmap_word_t) 0
                                              : ((bitmap_word_t) 1 << left) - 1;
    }
}

static inline void bitmap_set_range(bitmap_word_t *map, size_t start,
                                    size_t len) {
    for (size_t i = 0; i < len; i++) {
        bitmap_set(map, start + i);
    }
}

static inline void bitmap_clear_range(bitmap_word_t *map, size_t start,
                                      size_t len) {
    for (size_t i = 0; i < len; i++) {
        bitmap_clear(map, start + i);
    }
}

static inline void bitmap_and(bitmap_word_t *dst, const bitmap_word_t *src1,
                              const bitmap_word_t *src2, size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    for (size_t i = 0; i < words; i++) {
        dst[i] = src1[i] & src2[i];
    }
}

static inline void bitmap_or(bitmap_word_t *dst, const bitmap_word_t *src1,
                             const bitmap_word_t *src2, size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    for (size_t i = 0; i < words; i++) {
        dst[i] = src1[i] | src2[i];
    }
}

static inline void bitmap_xor(bitmap_word_t *dst, const bitmap_word_t *src1,
                              const bitmap_word_t *src2, size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    for (size_t i = 0; i < words; i++) {
        dst[i] = src1[i] ^ src2[i];
    }
}

static inline void bitmap_andnot(bitmap_word_t *dst, const bitmap_word_t *src1,
                                 const bitmap_word_t *src2, size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    for (size_t i = 0; i < words; i++) {
        dst[i] = src1[i] & ~src2[i];
    }
}

static inline bool bitmap_equal(const bitmap_word_t *src1,
                                const bitmap_word_t *src2, size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    for (size_t i = 0; i < words; i++) {
        if (src1[i] != src2[i]) {
            return false;
        }
    }
    return true;
}

static inline bool bitmap_intersects(const bitmap_word_t *src1,
                                     const bitmap_word_t *src2, size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    for (size_t i = 0; i < words; i++) {
        if ((src1[i] & src2[i]) != 0) {
            return true;
        }
    }
    return false;
}

static inline bool bitmap_subset(const bitmap_word_t *subset,
                                 const bitmap_word_t *superset, size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    for (size_t i = 0; i < words; i++) {
        if ((subset[i] & ~superset[i]) != 0) {
            return false;
        }
    }
    return true;
}

static inline bool bitmap_empty(const bitmap_word_t *map, size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    for (size_t i = 0; i < words; i++) {
        if (map[i] != 0) {
            return false;
        }
    }
    return true;
}

static inline bool bitmap_full(const bitmap_word_t *map, size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    if (words == 0)
        return true;
    for (size_t i = 0; i < words - 1; i++) {
        if (map[i] != ~(bitmap_word_t) 0) {
            return false;
        }
    }
    size_t rem = nbits % BITMAP_BITS_PER_WORD;
    bitmap_word_t mask =
        rem ? (((bitmap_word_t) 1 << rem) - 1) : ~(bitmap_word_t) 0;
    return (map[words - 1] & mask) == mask;
}

static inline size_t bitmap_weight(const bitmap_word_t *map, size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    size_t count = 0;

    if (words == 0) {
        return 0;
    }

    for (size_t i = 0; i < words - 1; i++) {
        count += (size_t) ci_popcountll(map[i]);
    }

    size_t rem = nbits % BITMAP_BITS_PER_WORD;
    bitmap_word_t last = map[words - 1];
    if (rem) {
        last &= ((bitmap_word_t) 1 << rem) - 1;
    }
    count += (size_t) ci_popcountll(last);

    return count;
}

static inline size_t bitmap_find_first_set(const bitmap_word_t *map,
                                           size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    for (size_t i = 0; i < words; i++) {
        if (map[i]) {
            size_t bit = i * BITMAP_BITS_PER_WORD + (size_t) ci_ctzll(map[i]);
            return MIN(bit, nbits);
        }
    }
    return nbits;
}

static inline size_t bitmap_find_first_zero(const bitmap_word_t *map,
                                            size_t nbits) {
    size_t words = BITMAP_WORDS(nbits);
    for (size_t i = 0; i < words; i++) {
        bitmap_word_t inv = ~map[i];
        if (inv) {
            size_t bit = i * BITMAP_BITS_PER_WORD + (size_t) ci_ctzll(inv);
            return MIN(bit, nbits);
        }
    }
    return nbits;
}

static inline size_t bitmap_find_next_bit(const bitmap_word_t *map,
                                          size_t nbits, size_t start) {
    if (start >= nbits) {
        return nbits;
    }

    size_t word_index = BITMAP_WORD_INDEX(start);
    size_t bit_offset = BITMAP_BIT_OFFSET(start);

    bitmap_word_t word = map[word_index] & (~((bitmap_word_t) 0) << bit_offset);
    if (word) {
        size_t bit =
            word_index * BITMAP_BITS_PER_WORD + (size_t) ci_ctzll(word);
        return MIN(bit, nbits);
    }

    for (size_t i = word_index + 1; i < BITMAP_WORDS(nbits); i++) {
        if (map[i]) {
            size_t bit = i * BITMAP_BITS_PER_WORD + (size_t) ci_ctzll(map[i]);
            return MIN(bit, nbits);
        }
    }

    return nbits;
}

static inline size_t bitmap_find_next_zero_bit(const bitmap_word_t *map,
                                               size_t nbits, size_t start) {
    if (start >= nbits) {
        return nbits;
    }

    size_t word_index = BITMAP_WORD_INDEX(start);
    size_t bit_offset = BITMAP_BIT_OFFSET(start);

    bitmap_word_t inv = ~map[word_index] & (~((bitmap_word_t) 0) << bit_offset);
    if (inv) {
        size_t bit = word_index * BITMAP_BITS_PER_WORD + (size_t) ci_ctzll(inv);
        return MIN(bit, nbits);
    }

    for (size_t i = word_index + 1; i < BITMAP_WORDS(nbits); i++) {
        inv = ~map[i];
        if (inv) {
            size_t bit = i * BITMAP_BITS_PER_WORD + (size_t) ci_ctzll(inv);
            return MIN(bit, nbits);
        }
    }

    return nbits;
}
