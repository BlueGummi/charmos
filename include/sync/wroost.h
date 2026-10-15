/* @title: Writer to Reader Boosting */
#pragma once
#include <math/hash.h>
#include <thread/thread_types.h>

/*
 * wroost "writer reader boost" portmanteau
 *
 * Implementing writer to reader boosting for rwlocks is quite tricky,
 * especially for turnstile-backed ones where you don't get extra data
 * to store lists of threads.
 *
 *
 *
 *
 */

struct rwlock;
#define WROOST_BITS_PER_COUNTER 2
#define WROOST_COUNTER_MAX 0x3
#define WROOST_COUNTER_MASK 0x3
#define WROOST_BUCKET_COUNT 256
#define WROOST_THREAD_COUNTERS 64
#define WROOST_EXACT_ENTRY_COUNT 4
#define WROOST_THREAD_COUNTER_BITS WROOST_THREAD_COUNTERS * 2
#define WROOST_THREAD_COUNTER_BYTES (WROOST_THREAD_COUNTER_BITS / BITS_PER_BYTE)

typedef uint8_t wroost_id_t;
typedef uint8_t wroost_counter_t;

static_assert(BITS_NEEDED(WROOST_THREAD_COUNTERS - 1) +
                  BITS_NEEDED((WROOST_BUCKET_COUNT / WROOST_THREAD_COUNTERS) -
                              1) ==
              8);
static_assert(BITS_NEEDED(WROOST_THREAD_COUNTERS - 1) == 6);

static_assert(BIT_SIZEOF(wroost_id_t) >= BITS_NEEDED(WROOST_THREAD_COUNTERS),
              "wroost_id_t needs to be wider");

struct wroost_exact_entry {
    uint8_t offset : 2;
    uint8_t index : 6;
};
ct_assert_struct_size_eq(wroost_exact_entry, 1);

struct wroost_thread_state {
    /* Reader */
    wroost_counter_t counters[WROOST_THREAD_COUNTER_BYTES];
    struct wroost_exact_entry exact_entries[WROOST_EXACT_ENTRY_COUNT];
    uint32_t nesting_depth;

    /* Writer */
    wroost_id_t lock_hash;
};
ct_assert_struct_size_eq(wroost_thread_state, 28);

struct wroost_bucket {
    union {
        /* "tally", so as to not conflict with "counter" */
        atomic_uint16_t owner_tally[THREAD_PRIO_CLASS_COUNT];
        atomic_uint64_t raw;
    };
};

struct wroost_hash_table {
    struct wroost_bucket buckets[WROOST_BUCKET_COUNT];
};

static inline wroost_id_t wroost_hash_lock(struct rwlock *lock) {
    return hash_pointer(lock) % WROOST_BUCKET_COUNT;
}

/* Counters are two bits. Index 0 is bit 0, index 1 is bit 2, index 2 is bit 4, etc. */
static inline uint8_t wroost_get_counter(struct wroost_thread_state *wts,
                                         uint8_t index, uint8_t *byte_idx_out,
                                         uint8_t *byte_offset_out) {
    enforce(index < WROOST_THREAD_COUNTERS);
    uint8_t bits_per_ctr_word = BIT_SIZEOF(wroost_counter_t);
    size_t bit_index = index * WROOST_BITS_PER_COUNTER;
    *byte_idx_out = bit_index / bits_per_ctr_word;
    *byte_offset_out = bit_index % bits_per_ctr_word;
    return wts->counters[*byte_idx_out];
}

static inline wroost_counter_t
wroost_read_counter(struct wroost_thread_state *wts, uint8_t ctr_idx) {
    uint8_t idx, offset;
    wroost_counter_t ctr = wroost_get_counter(wts, ctr_idx, &idx, &offset);
    return (ctr >> offset) & WROOST_COUNTER_MASK;
}

static inline uint8_t wroost_inc_counter(struct wroost_thread_state *wts,
                                         uint8_t ctr_idx) {
    if (wroost_read_counter(wts, ctr_idx) == WROOST_COUNTER_MAX)
        return WROOST_COUNTER_MAX;

    uint8_t idx, offset;
    wroost_counter_t ctr = wroost_get_counter(wts, ctr_idx, &idx, &offset);
    wroost_counter_t new_val = ((ctr >> offset) & WROOST_COUNTER_MASK) + 1;
    wroost_counter_t cleared = ctr & ~(WROOST_COUNTER_MASK << offset);
    wroost_counter_t new = (new_val << offset) | cleared;
    wts->counters[idx] = new;
    return new_val;
}

static inline struct wroost_exact_entry *
wroost_get_exact_entry(struct wroost_thread_state *wts, uint8_t index) {
    enforce(index < WROOST_THREAD_COUNTERS);
    for (int i = 0; i < WROOST_EXACT_ENTRY_COUNT; i++) {
        if (wts->exact_entries[i].index == index)
            return &wts->exact_entries[i];
    }

    return NULL;
}

static inline void wroost_clear_exact_entry(struct wroost_exact_entry *ent) {
    ent->index = 0;
    ent->offset = 0;
}

static inline struct wroost_exact_entry *
wroost_alloc_exact_entry(struct wroost_thread_state *wts) {
    for (int i = 0; i < WROOST_EXACT_ENTRY_COUNT; i++) {
        if (wts->exact_entries[i].index != 0) {
            if ((wts->counters[0] & WROOST_COUNTER_MASK) == 0)
                return &wts->exact_entries[i];
        }
    }

    return NULL;
}

static inline void wroost_move_one_tally(struct wroost_bucket *bkt,
                                         enum thread_prio_class from,
                                         enum thread_prio_class to) {
#ifdef DEBUG_WROOST
    for (int i = 0; i < THREAD_PRIO_CLASS_COUNT; i++)
        enforce(atomic_load_rlx(&bkt->owner_tally[i]) != UINT16_MAX);
#endif

    uint64_t inc_mask = 1ULL << (to * BIT_SIZEOF(uint16_t));
    uint64_t dec_mask = 1ULL << (from * BIT_SIZEOF(uint16_t));
    uint64_t delta = inc_mask - dec_mask;
    atomic_fetch_add_acq_rel(&bkt->raw, delta);
}
