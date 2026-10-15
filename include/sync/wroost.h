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
#define WROOST_BUCKET_COUNT 256
#define WROOST_THREAD_COUNTERS 64
#define WROOST_EXACT_ENTRY_COUNT 4
#define WROOST_THREAD_COUNTER_BITS WROOST_THREAD_COUNTERS * 2
#define WROOST_THREAD_COUNTER_BYTES (WROOST_THREAD_COUNTER_BITS / BITS_PER_BYTE)

/* 3 because only "urgent" is not a valid prior state. Only
 * BACKGROUND, TIMESHARE, and REALTIME are valid for before_boost states. */
#define WROOST_THREAD_INDEX_ASSOCS 3

typedef uint8_t wroost_id_t;

static_assert(BITS_NEEDED(WROOST_THREAD_COUNTERS - 1) +
                  BITS_NEEDED((WROOST_BUCKET_COUNT / WROOST_THREAD_COUNTERS) -
                              1) ==
              8);
static_assert(BITS_NEEDED(WROOST_THREAD_COUNTERS - 1) == 6);

static_assert(BITS_PER_BYTE * sizeof(wroost_id_t) >=
                  BITS_NEEDED(WROOST_THREAD_COUNTERS),
              "wroost_id_t needs to be wider");

struct wroost_exact_entry {
    uint8_t offset : 2;
    uint8_t index : 6;
};
ct_assert_struct_size_eq(wroost_exact_entry, 1);

struct wroost_index_assoc {
    uint8_t index : 6;
    enum thread_prio_class before_boost : 2;
};
ct_assert_struct_size_eq(wroost_index_assoc, 1);

struct wroost_thread_state {
    /* Reader */
    uint8_t counters[WROOST_THREAD_COUNTER_BYTES];
    struct wroost_exact_entry exact_entries[WROOST_EXACT_ENTRY_COUNT];
    uint32_t nesting_depth;

    /* Writer */
    wroost_id_t lock_hash;

    /* Boost data */
    struct wroost_index_assoc index_assocs[WROOST_THREAD_INDEX_ASSOCS];
};
ct_assert_struct_size_eq(wroost_thread_state, 28);

struct wroost_bucket {
    atomic_uint16_t owner_counts[THREAD_PRIO_CLASS_COUNT];
};

struct wroost_hash_table {
    struct wroost_bucket buckets[WROOST_BUCKET_COUNT];
};

static inline wroost_id_t wroost_hash_lock(struct rwlock *lock) {
    return hash_pointer(lock) % WROOST_BUCKET_COUNT;
}

static inline uint8_t wroost_read_counter(struct wroost_thread_state *wts,
                                          uint8_t index) {
    enforce(index < WROOST_THREAD_COUNTERS);
    unimplemented();
}

static inline struct wroost_exact_entry *
wroost_get_exact_entry(struct wroost_thread_state *wts, uint8_t index) {
    for (int i = 0; i < WROOST_EXACT_ENTRY_COUNT; i++) {
        if (wts->exact_entries[i].index == index)
            return &wts->exact_entries[i];
    }

    return NULL;
}

static inline struct wroost_exact_entry *
wroost_set_exact_entry(struct wroost_thread_state *wts, uint8_t index,
                       uint8_t offset) {
    unimplemented();
    for (int i = 0; i < WROOST_EXACT_ENTRY_COUNT; i++) {}
}
