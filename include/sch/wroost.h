/* @title: Writer to Reader Boosting */
#pragma once
#include <math/hash.h>
#include <structures/hlist.h>
#include <thread/thread_types.h>

/*
 * wroost "writer reader boost" portmanteau
 *
 * Implementing writer to reader boosting for rwlocks is quite tricky,
 * especially for turnstile-backed ones where you don't get extra data
 * to store lists of threads.
 *
 * Lock side stuff:
 *
 * Thus, this is a sort of lazy priority ceiling. The idea is this:
 * we store a hash table where each bucket has no hlist, but rather,
 * keeps a set of 4 16-bit counters and a CPU mask.
 *
 * We hash every lock to store a small 'counting bloom filter'
 * within each thread. We also keep a max_boost which is the
 * maximum of all rwlock's maximum priorities to mitigate excess
 * boosting that could be pathological.
 *
 * Then, writers hash into the table and bump the counter that
 * corresponds to their perceived priority.
 *
 * Scheduler side stuff:
 *
 * Every CPU stores 256 x 16 bit counters that indicate the number of threads
 * with a given ID (coarse entries are fanned across their respective
 * sub-buckets) read-locked.
 *
 * When one of these counters passes the 0 <-> 1 transition boundary, it
 * sets/clears a CPU mask in the corresponding global hash table bucket.
 *
 * Migrations also alter the CPU masks if they cross a transition boundary,
 * and do so by first setting the new bit and then clearing the old one.
 *
 *
 *
 *
 *
 */

struct rwlock;
/* Thread data */
#define WROOST_BITS_PER_COUNTER 2
#define WROOST_THREAD_MAP_DEPTH 32
#define WROOST_COUNTER_MAX 0x3
#define WROOST_COUNTER_MASK 0x3

#define WROOST_FINE_INDEX_SHIFT 3
#define WROOST_FINE_OFFSET_MASK 0x7

#define WROOST_BUCKET_COUNT 256
#define WROOST_THREAD_COUNTERS 32
#define WROOST_COARSE_FACTOR (WROOST_BUCKET_COUNT / WROOST_THREAD_COUNTERS)
#define WROOST_FINE_ENTRY_COUNT 4
#define WROOST_FINE_VALID_MASK ((1U << WROOST_FINE_ENTRY_COUNT) - 1U)
#define WROOST_THREAD_COUNTER_BITS (WROOST_THREAD_COUNTERS * 2)
#define WROOST_THREAD_COUNTER_BYTES (WROOST_THREAD_COUNTER_BITS / BITS_PER_BYTE)

/* Percpu data */
#define WROOST_PENDING_BITMAP_WORD_SHIFT 6
#define WROOST_THREAD_MAP_OVERFLOW ((struct thread *) (UINTPTR_MAX))

typedef uint8_t wroost_id_t;
typedef uint8_t wroost_counter_t;

static_assert(BITS_NEEDED(WROOST_THREAD_COUNTERS - 1) +
                  BITS_NEEDED((WROOST_BUCKET_COUNT / WROOST_THREAD_COUNTERS) -
                              1) ==
              8);
static_assert(BITS_NEEDED(WROOST_THREAD_COUNTERS - 1) == 5);

static_assert(BIT_SIZEOF(wroost_id_t) >= BITS_NEEDED(WROOST_THREAD_COUNTERS),
              "wroost_id_t needs to be wider");

enum wroost_state {
    WROOST_DPC_IDLE = 0,
    WROOST_DPC_PENDING = 1,
    WROOST_DPC_ACTIVE = 1 << 1,
};

/* Protected by scheduler->lock */
struct wroost_percpu_data {
    atomic(enum wroost_state) state;
    cpu_id_t id;

    /* This keeps track of the number of READY threads on this CPU's
     * runqueues of every given lock ID. Notably, it does NOT
     * keep track of RUNNING threads OR threads that were going to block.
     *
     * Upon switch in, if a thread has any rwlocks held as a reader,
     * it "contributes", and when leaving, it "withdraws" its counts
     * added.
     *
     * However, the threads only contribute 1 per lock maximum,
     * even if its 2-bit counter is more than 1. The intent here is to
     * simply track "any thread", not "exact counter", so we only
     * need every thread to contribute 1 for the count if it has
     * that lock ID held as a reader.
     */
    uint16_t rq_bucket_counts[WROOST_BUCKET_COUNT];

    /* Not actually a hashmap, it's just the number of counters,
     * where precise entries get their coarse entry counter */
    struct hlist_head thread_map[WROOST_THREAD_COUNTERS];

    /* Since thread_map is guaranteed to only have single lock readers */
    struct hlist_head multi_lock_readers;
    BITMAP_DECLARE_ATOMIC(pending_bitmap, WROOST_BUCKET_COUNT);
    struct dpc boost_dpc;
};

struct wroost_fine_entry {
    wroost_id_t lock_id; /* Shift this right by WROOST_FINE_INDEX_SHIFT
                          * to get the coarse index of the fine entry */
};
ct_assert_struct_size_eq(wroost_fine_entry, 1);

/*
 * There's a key invariant with fine_entries:
 *
 * ====================================================================
 * A counter is either FULLY represented in fine_entries, or completely
 * unrepresented within fine_entries.
 * ====================================================================
 *
 * The reason why:
 *
 * If 3/4 fine_entries are taken up, and a counter wants to allocate
 * two more, then it ends up with a split: one of its contributing
 * numbers in the counter is in the fine_entries, and one isn't.
 *
 * This means that the boost HAS to use the coarse entry, since
 * otherwise, we risk a false negative, which would be completely
 * against the premise of wroost.
 *
 * Thus, that extra fine_entry is wasted on a lock ID that doesn't
 * use it. So, this invariant keeps all fine entries useful.
 */
struct wroost_thread_data {
    struct hlist_node map_node;

    /* Reader */
    wroost_counter_t counters[WROOST_THREAD_COUNTER_BYTES];
    struct wroost_fine_entry fine_entries[WROOST_FINE_ENTRY_COUNT];
    uint8_t read_nesting_depth;

    /* Writer */
    wroost_id_t want_lock_id;
    bool writer_skip : 1;
    bool writer_want : 1; /* Is a writer too... scheduler
                           * should consider the want_lock_id.
                           *
                           * This is what we use to determine
                           * if the thread is a writer,
                           * since want_lock_id == 0 is a valid writer
                           * state and we can't use that to guard */

    /* boost_class_ceil is the ceil that the writer->reader boosts contribute.
     * There is only one, and it is the maximum of any of the lock IDs it holds.
     *
     * max_boost serves a completely different role. Every rwlock is
     * instantiated with a fixed maximum on the highest thread priority class
     * that can enter the lock. max_boost is the maximum of all of these.
     * The idea is that if an URGENT thread blocks on a rwlock with a maximum
     * of RT, that code will panic.
     *
     * Thus, if we encounter a thread that seems
     * to hold an ID where the urgent counter is >= 1, if this max_boost
     * is at RT, we actually *don't* boost to urgent, and boost to what's
     * further down (not necessarily RT either). It's essentially a way
     * to cheaply verify that a big boost does NOT belong to us,
     * because if it did, the kernel would've crashed long before.
     */
    enum thread_prio_class max_boost : THREAD_PRIO_CLASS_BIT_COUNT;

    bool contributed_boost : 1; /* Set by scheduler */

    /* Tracks which of the 4 fine_entries are active so index == 0
     * does not get interpreted being a free slot  */
    uint8_t fine_valid : WROOST_FINE_ENTRY_COUNT;

    enum thread_prio_class boost_class_ceil : THREAD_PRIO_CLASS_BIT_COUNT;
};
ct_assert_struct_size_eq(wroost_thread_data, 32);

struct wroost_bucket {
    /* "tally", so as to not conflict with "counter":
     *
     * 16-bit 2 * prio_class offset counters of owner prios */
    atomic_uint64_t raw_tally;

    /* 2 bits per CPU for 2 bits per prio class */

    /* clang-format is very eager to format this as a pointer (??)
     * TODO: Maybe this is a bug upstream */

    // clang-format off
    BITMAP_DECLARE_ATOMIC(rq_min_prio, THREAD_PRIO_CLASS_BIT_COUNT * CPU_MASK_BITS);
    // clang-format on
};

struct wroost_hash_table {
    struct wroost_bucket buckets[WROOST_BUCKET_COUNT];
};

static inline wroost_id_t wroost_hash(struct rwlock *lock) {
    return hash_pointer(lock) % WROOST_BUCKET_COUNT;
}

/* Counters are two bits. Index 0 is bit 0, index 1 is bit 2, index 2 is bit 4, etc. */
static inline uint8_t wroost_get_counter(struct wroost_thread_data *wts,
                                         uint8_t index, uint8_t *byte_idx_out,
                                         uint8_t *byte_offset_out) {
    enforce(index < WROOST_THREAD_COUNTERS);
    uint8_t bits_per_ctr_word = BIT_SIZEOF(wroost_counter_t);
    size_t bit_index = index * WROOST_BITS_PER_COUNTER;

    if (byte_idx_out)
        *byte_idx_out = bit_index / bits_per_ctr_word;

    if (byte_offset_out)
        *byte_offset_out = bit_index % bits_per_ctr_word;

    return wts->counters[bit_index / bits_per_ctr_word];
}

static inline wroost_counter_t
wroost_read_counter(struct wroost_thread_data *wts, uint8_t ctr_idx) {
    uint8_t idx, offset;
    wroost_counter_t ctr = wroost_get_counter(wts, ctr_idx, &idx, &offset);
    return (ctr >> offset) & WROOST_COUNTER_MASK;
}

static inline void wroost_write_counter(struct wroost_thread_data *wts,
                                        uint8_t ctr_idx, uint8_t new_ctr_val) {
    enforce((new_ctr_val & WROOST_COUNTER_MASK) == new_ctr_val);

    uint8_t idx, offset;
    wroost_counter_t ctr = wroost_get_counter(wts, ctr_idx, &idx, &offset);
    wroost_counter_t cleared = ctr & ~(WROOST_COUNTER_MASK << offset);
    wroost_counter_t new = (new_ctr_val << offset) | cleared;
    wts->counters[idx] = new;
}

static inline uint8_t wroost_inc_counter(struct wroost_thread_data *wts,
                                         uint8_t ctr_idx,
                                         wroost_counter_t *ctr_val) {
    if ((*ctr_val = wroost_read_counter(wts, ctr_idx)) == WROOST_COUNTER_MAX)
        return WROOST_COUNTER_MAX;

    wroost_write_counter(wts, ctr_idx, *ctr_val + 1);
    return *ctr_val + 1;
}

static inline uint8_t wroost_dec_counter(struct wroost_thread_data *wts,
                                         uint8_t ctr_idx) {
    wroost_counter_t ctr_val;
    if ((ctr_val = wroost_read_counter(wts, ctr_idx)) == WROOST_COUNTER_MAX)
        return WROOST_COUNTER_MAX;

    enforce(ctr_val > 0);
    wroost_write_counter(wts, ctr_idx, ctr_val - 1);
    return ctr_val - 1;
}

static inline uint8_t wroost_fine_entry_index(struct wroost_fine_entry *ent) {
    return ent->lock_id >> WROOST_FINE_INDEX_SHIFT;
}

static inline struct wroost_fine_entry *
wroost_get_fine_entry(struct wroost_thread_data *wts, uint8_t index) {
    enforce(index < WROOST_THREAD_COUNTERS);
    for (int i = 0; i < WROOST_FINE_ENTRY_COUNT; i++) {
        if ((wts->fine_valid & BIT(i)) &&
            wroost_fine_entry_index(&wts->fine_entries[i]) == index)
            return &wts->fine_entries[i];
    }

    return NULL;
}

static inline void wroost_clear_fine_entry(struct wroost_thread_data *wts,
                                           int slot) {
    enforce(slot >= 0 && slot < WROOST_FINE_ENTRY_COUNT);
    wts->fine_entries[slot].lock_id = 0;
    wts->fine_valid &= ~BIT(slot);
}

static inline void wroost_evict_fine_entries(struct wroost_thread_data *wts,
                                             uint8_t index) {
    for (int i = 0; i < WROOST_FINE_ENTRY_COUNT; i++) {
        if ((wts->fine_valid & BIT(i)) &&
            wroost_fine_entry_index(&wts->fine_entries[i]) == index)
            wroost_clear_fine_entry(wts, i);
    }
}

static inline struct wroost_fine_entry *
wroost_alloc_fine_entry(struct wroost_thread_data *wts) {
    /* Notable edge case here:
     *
     * the index can be 0, so we track slot liveness explicitly via fine_valid
     * instead of relying on index == 0 meaning 'free'.
     */
    uint8_t free_mask = (~wts->fine_valid) & WROOST_FINE_VALID_MASK;
    if (free_mask == 0)
        return NULL;

    int i = ci_ctz(free_mask);
    wts->fine_valid |= BIT(i);
    return &wts->fine_entries[i];
}

static inline uint16_t wroost_read_tally(struct wroost_bucket *bkt,
                                         enum thread_prio_class class) {
    enforce(class < THREAD_PRIO_CLASS_COUNT);
    uint64_t ret = atomic_load_acq(&bkt->raw_tally);
    return (ret >> ((size_t) class * BIT_SIZEOF(uint16_t))) & UINT16_MAX;
}

/* TODO: Although it is virtually impossible,
 * this could overflow at 65536 */
static inline void wroost_move_tally(struct wroost_bucket *bkt,
                                     enum thread_prio_class from,
                                     enum thread_prio_class to) {
#ifdef DEBUG_WROOST
    for (int i = 0; i < THREAD_PRIO_CLASS_COUNT; i++)
        enforce(wroost_read_tally(bkt, i) != UINT16_MAX);
#endif

    uint64_t inc_mask = BIT((size_t) to * BIT_SIZEOF(uint16_t));
    uint64_t dec_mask = BIT((size_t) from * BIT_SIZEOF(uint16_t));
    uint64_t delta = inc_mask - dec_mask;
    atomic_fetch_add_release(&bkt->raw_tally, delta);
}

enum thread_prio_class wroost_compute_ceil(struct wroost_thread_data *wts,
                                           enum thread_prio_class min);
void wroost_post_read_release(struct rwlock *lock);
void wroost_post_read_acquire(struct rwlock *lock,
                              enum thread_prio_class lock_max_ceil);
void wroost_scheduler_init(struct scheduler *sched);
