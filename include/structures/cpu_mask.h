/* @title: CPU Mask */
#pragma once
#include <math/div.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <structures/bitmap.h>

#ifndef CPU_MASK_BITS
#define CPU_MASK_BITS 128
#endif

#define CPU_MASK_WORD_BITS 64
#define CPU_MASK_WORDS BITMAP_WORDS(CPU_MASK_BITS)

struct cpu_mask {
    bitmap_word_t bits[CPU_MASK_WORDS];
};

#define CPU_MASK_INIT                                                          \
    (struct cpu_mask) {                                                        \
        .bits = {0}                                                            \
    }

/* Used to overload cpu_mask to carry an error at times */
#define CPU_MASK_ERR(e)                                                        \
    (struct cpu_mask) {                                                        \
        .bits = {(bitmap_word_t) (e)}                                          \
    }

static inline bool cpu_mask_init(struct cpu_mask *m, size_t nbits) {
    (void) nbits;
    memset(m->bits, 0, sizeof(m->bits));
    return true;
}

static inline void cpu_mask_deinit(struct cpu_mask *m) {
    (void) m;
}

struct cpu_mask *cpu_mask_create(void);
void cpu_mask_free(struct cpu_mask *m);

static inline void cpu_mask_copy(struct cpu_mask *dst,
                                 const struct cpu_mask *src) {
    *dst = *src;
}

static inline void cpu_mask_set(struct cpu_mask *m, size_t cpu) {
    if (cpu < CPU_MASK_BITS) {
        bitmap_set(m->bits, cpu);
    }
}

static inline void cpu_mask_clear(struct cpu_mask *m, size_t cpu) {
    if (cpu < CPU_MASK_BITS) {
        bitmap_clear(m->bits, cpu);
    }
}

static inline void cpu_mask_toggle(struct cpu_mask *m, size_t cpu) {
    if (cpu < CPU_MASK_BITS) {
        bitmap_toggle(m->bits, cpu);
    }
}

static inline bool cpu_mask_test(const struct cpu_mask *m, size_t cpu) {
    return (cpu < CPU_MASK_BITS) ? bitmap_test(m->bits, cpu) : false;
}

static inline bool cpu_mask_test_and_set(struct cpu_mask *m, size_t cpu) {
    return (cpu < CPU_MASK_BITS) ? bitmap_test_and_set(m->bits, cpu) : false;
}

static inline bool cpu_mask_test_and_clear(struct cpu_mask *m, size_t cpu) {
    return (cpu < CPU_MASK_BITS) ? bitmap_test_and_clear(m->bits, cpu) : false;
}

static inline void cpu_mask_set_atomic(struct cpu_mask *m, size_t cpu) {
    if (cpu < CPU_MASK_BITS) {
        bitmap_atomic_set(m->bits, cpu);
    }
}

static inline void cpu_mask_clear_atomic(struct cpu_mask *m, size_t cpu) {
    if (cpu < CPU_MASK_BITS) {
        bitmap_atomic_clear(m->bits, cpu);
    }
}

static inline void cpu_mask_toggle_atomic(struct cpu_mask *m, size_t cpu) {
    if (cpu < CPU_MASK_BITS) {
        bitmap_atomic_toggle(m->bits, cpu);
    }
}

static inline bool cpu_mask_test_atomic(const struct cpu_mask *m, size_t cpu) {
    return (cpu < CPU_MASK_BITS) ? bitmap_atomic_test(m->bits, cpu) : false;
}

static inline bool cpu_mask_test_and_set_atomic(struct cpu_mask *m,
                                                size_t cpu) {
    return (cpu < CPU_MASK_BITS) ? bitmap_atomic_test_and_set(m->bits, cpu)
                                 : false;
}

static inline bool cpu_mask_test_and_clear_atomic(struct cpu_mask *m,
                                                  size_t cpu) {
    return (cpu < CPU_MASK_BITS) ? bitmap_atomic_test_and_clear(m->bits, cpu)
                                 : false;
}

static inline void cpu_mask_clear_all(struct cpu_mask *m) {
    bitmap_zero(m->bits, CPU_MASK_BITS);
}

static inline void cpu_mask_zero(struct cpu_mask *m) {
    bitmap_zero(m->bits, CPU_MASK_BITS);
}

static inline void cpu_mask_fill(struct cpu_mask *m) {
    bitmap_fill(m->bits, CPU_MASK_BITS);
}

#define cpu_mask_active_bits()                                                 \
    (global.core_count ? global.core_count : CPU_MASK_BITS)

#define cpu_mask_set_all(m) bitmap_fill((m)->bits, cpu_mask_active_bits())

static inline void cpu_mask_set_range(struct cpu_mask *m, size_t start,
                                      size_t len) {
    bitmap_set_range(m->bits, start, len);
}

static inline void cpu_mask_clear_range(struct cpu_mask *m, size_t start,
                                        size_t len) {
    bitmap_clear_range(m->bits, start, len);
}

static inline void cpu_mask_or(struct cpu_mask *dst, const struct cpu_mask *b) {
    bitmap_or(dst->bits, dst->bits, b->bits, CPU_MASK_BITS);
}

static inline void cpu_mask_or2(struct cpu_mask *dst, const struct cpu_mask *a,
                                const struct cpu_mask *b) {
    bitmap_or(dst->bits, a->bits, b->bits, CPU_MASK_BITS);
}

static inline void cpu_mask_and(struct cpu_mask *dst, const struct cpu_mask *a,
                                const struct cpu_mask *b) {
    bitmap_and(dst->bits, a->bits, b->bits, CPU_MASK_BITS);
}

static inline void cpu_mask_xor(struct cpu_mask *dst, const struct cpu_mask *a,
                                const struct cpu_mask *b) {
    bitmap_xor(dst->bits, a->bits, b->bits, CPU_MASK_BITS);
}

static inline void cpu_mask_andnot(struct cpu_mask *dst,
                                   const struct cpu_mask *a,
                                   const struct cpu_mask *b) {
    bitmap_andnot(dst->bits, a->bits, b->bits, CPU_MASK_BITS);
}

static inline bool cpu_mask_intersects(const struct cpu_mask *a,
                                       const struct cpu_mask *b) {
    return bitmap_intersects(a->bits, b->bits, CPU_MASK_BITS);
}

static inline bool cpu_mask_equal(const struct cpu_mask *a,
                                  const struct cpu_mask *b) {
    return bitmap_equal(a->bits, b->bits, CPU_MASK_BITS);
}

static inline bool cpu_mask_subset(const struct cpu_mask *subset,
                                   const struct cpu_mask *superset) {
    return bitmap_subset(subset->bits, superset->bits, CPU_MASK_BITS);
}

static inline size_t cpu_mask_popcount(const struct cpu_mask *m) {
    return bitmap_weight(m->bits, CPU_MASK_BITS);
}

static inline size_t cpu_mask_weight(const struct cpu_mask *m) {
    return bitmap_weight(m->bits, CPU_MASK_BITS);
}

static inline bool cpu_mask_empty(const struct cpu_mask *m) {
    return bitmap_empty(m->bits, CPU_MASK_BITS);
}

static inline bool cpu_mask_full(const struct cpu_mask *m) {
    return bitmap_full(m->bits, CPU_MASK_BITS);
}

static inline size_t cpu_mask_first_set(const struct cpu_mask *m) {
    return bitmap_find_first_set(m->bits, CPU_MASK_BITS);
}

static inline size_t cpu_mask_first_clear(const struct cpu_mask *m) {
    return bitmap_find_first_zero(m->bits, CPU_MASK_BITS);
}

static inline size_t cpu_mask_next_set(const struct cpu_mask *m, size_t start) {
    return bitmap_find_next_bit(m->bits, CPU_MASK_BITS, start);
}

static inline size_t cpu_mask_next_clear(const struct cpu_mask *m,
                                         size_t start) {
    return bitmap_find_next_zero_bit(m->bits, CPU_MASK_BITS, start);
}

#define for_each_cpu(cpu, mask_ptr)                                            \
    for ((cpu) =                                                               \
             bitmap_find_first_set((mask_ptr)->bits, cpu_mask_active_bits());  \
         (cpu) < cpu_mask_active_bits();                                       \
         (cpu) = bitmap_find_next_bit((mask_ptr)->bits,                        \
                                      cpu_mask_active_bits(), (cpu) + 1))

#define cpu_mask_for_each(iter, mask)                                          \
    for (iter = 0; iter < cpu_mask_active_bits(); iter++)                      \
        if (bitmap_test((mask).bits, iter))

#define cpu_mask_for_each_clear(iter, mask)                                    \
    for (iter = 0; iter < cpu_mask_active_bits(); iter++)                      \
        if (!bitmap_test((mask).bits, iter))

#define cpu_mask_for_all(iter, mask)                                           \
    for (iter = 0; iter < cpu_mask_active_bits(); iter++)

#define cpu_mask_for_each_in(iter, mask, start, end)                           \
    for (iter = (start); iter < cpu_mask_active_bits() && iter <= (end);       \
         iter++)                                                               \
        if (bitmap_test((mask).bits, iter))

#define cpu_mask_for_each_clear_in(iter, mask, start, end)                     \
    for (iter = (start); iter < cpu_mask_active_bits() && iter <= (end);       \
         iter++)                                                               \
        if (!bitmap_test((mask).bits, iter))
