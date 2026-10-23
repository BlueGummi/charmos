/* @title: Writer to Reader Boosting */
#pragma once
#include <math/hash.h>
#include <structures/hlist.h>
#include <sync/rwlock_types.h>
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

/* Thread data */
#define WROOST_BITS_PER_COUNTER 2
#define WROOST_THREAD_MAP_DEPTH 32
#define WROOST_COUNTER_MAX 0x3
#define WROOST_COUNTER_MASK 0x3

#define WROOST_MIN_BUCKET_COUNT 256U
#define WROOST_MAX_BUCKET_COUNT 32768U
#define WROOST_BUCKETS_PER_CPU_MULT 16U
#define WROOST_THREAD_COUNTERS 32
#define WROOST_FINE_ENTRY_COUNT 4
#define WROOST_FINE_VALID_MASK ((1U << WROOST_FINE_ENTRY_COUNT) - 1U)
#define WROOST_THREAD_COUNTER_BITS (WROOST_THREAD_COUNTERS * 2)
#define WROOST_THREAD_COUNTER_BYTES (WROOST_THREAD_COUNTER_BITS / BITS_PER_BYTE)

/* Percpu data */
#define WROOST_THREAD_MAP_OVERFLOW ((struct thread *) (UINTPTR_MAX))

typedef uint16_t wroost_id_t;
typedef uint8_t wroost_counter_t;

static_assert(BITS_NEEDED(WROOST_THREAD_COUNTERS - 1) == 5);

static_assert(BIT_SIZEOF(wroost_id_t) >= BITS_NEEDED(WROOST_THREAD_COUNTERS),
              "wroost_id_t needs to be wider");

/* Writers are a simple state machine.
 *
 * NONE = nothing happening
 * WANT = "I want a lock"
 * WAIT = "I am waiting for a lock"
 * BUMP = "I have yielded and bumped counters"
 *
 * WANT and WAIT are set in the thread's context, and BUMP is set immediately
 * after the WAIT reaches the switch out function.
 *
 * We leave it at BUMP throughout a critical section,*/
enum wroost_writer_state : uint8_t {
    WROOST_WRITER_NONE,
    WROOST_WRITER_WANT,
    WROOST_WRITER_WAIT,
    WROOST_WRITER_BUMP,
};

enum wroost_state {
    WROOST_DPC_IDLE = 0,
    WROOST_DPC_PENDING = 1,
    WROOST_DPC_ACTIVE = 1 << 1,
};

struct wroost_kicks {
    bool need_kicks;
    struct cpu_mask cpus;
    wroost_id_t lock_id;
};

struct wroost_process_result {
    bool restart;

    /* seen_data marks the lock IDs that the code has already
     * processed, at the bitmap word granularity. This is so
     * that if the pending process needs to restart, it
     * remains aware of what had already been processed, so
     * it can merge it with potentially new bits
     * and re-check and boost (since restart happens
     * due to a writer boosting a reader on calling CPU) */
    bitmap_word_t *seen_data;
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
     *
     * We need this because this is what allows us to make the
     * 0 <-> 1 switch for the bitmap per-lock-ID
     */
    uint8_t *rq_bucket_counts;

    /* Not actually a hashmap, it's just the number of counters,
     * where precise entries get their coarse entry counter */
    struct hlist_head thread_map[WROOST_THREAD_COUNTERS];

    /* Since thread_map is guaranteed to only have single lock readers */
    struct hlist_head multi_lock_readers;

    /* Readers that happen to be blocked on a turnstile go here. This is
     * because such readers need to propagate their priorities, which
     * cannot happen under sched->lock, but can under wroost_lock (and
     * that is the sole purpose that lock actually exists).
     *
     * Readers enter blocked_ts_readers on switch-out,
     * and exit it on switch-in, both of which cannot happen during
     * the DPC that iterates through this.
     *
     * Migrations are not a concern here because these threads do not
     * exist in the runqueues, and there is thus no way to have
     * obtained them for a migration. These threads must also
     * be woken on the CPU that they went to sleep on for this reason. */
    struct hlist_head blocked_ts_readers;
    atomic(bitmap_word_t) * pending_bitmap;

    struct wroost_process_result process_result;

    /* These primarily serve the purpose of implementing multi-hop.
     *
     * In the DPC, some readers are also writers, and upon a priority change,
     * they will also need to go bother relevant readers. However, this all
     * happens deep down in the scheduler lock, so DPCs can't be
     * arbitrarily sent (deadlock). Thus, we merge all the CPU masks into
     * a single one, and set bits in in_dpc_deferred_ids. The idea
     * is that merging all of this together means that we may
     * unnecessarily IPI some CPUs, but we will never have a false negative,
     * and in the common case, this won't be that bad since only a lock
     * or two of this kind will be held */
    bitmap_word_t *in_dpc_deferred_ids; /* Bits represent lock IDs */
    struct cpu_mask in_dpc_deferred_cpus;

    struct dpc boost_dpc;
    struct cpu_mask dpc_enqueue_deferred;
    wroost_id_t writer_lock_id; /* So we know what to set for deferred DPCS */
};

struct wroost_fine_entry {
    wroost_id_t lock_id; /* Shift this right by WROOST_FINE_INDEX_SHIFT
                          * to get the coarse index of the fine entry */
};

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

    enum wroost_writer_state writer_state;

    /* Boosts: applies to the readers */

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

    /* Whether this wroost_thread_data has
     * contributed to the thread's class_floors */
    bool contributed_floor : 1; /* Set by scheduler */
    bool in_blocked_ts_list : 1;

    /* TODO: use this to implement the optimization of knowing
     * the readers cannot be of lower priority */
    bool need_bump : 1;

    /* Tracks which of the 4 fine_entries are active so index == 0
     * does not get interpreted being a free slot  */
    uint8_t fine_valid : WROOST_FINE_ENTRY_COUNT;

    enum thread_prio_class floor : THREAD_PRIO_CLASS_BIT_COUNT;
};
ct_assert_struct_size_eq(wroost_thread_data, 40);

struct wroost_bucket {
    /* "tally", so as to not conflict with "counter":
     *
     * 16-bit 2 * prio_class offset counters of owner prios */
    atomic_uint64_t raw_tally;

    /* 2 bits per CPU for 2 bits per prio class */

    /* clang-format is very eager to format this as a pointer (??)
     * TODO: Maybe this is a bug upstream */

    /* clang-format off */
    BITMAP_DECLARE_ATOMIC(rq_min_prio, THREAD_PRIO_CLASS_BIT_COUNT * CPU_MASK_BITS);
    /* clang-format on */
};

struct wroost_hash_table {
    struct wroost_bucket *buckets;
    size_t bucket_count;
};

enum thread_prio_class wroost_compute_ceil(struct wroost_thread_data *wtd,
                                           enum thread_prio_class min);
void wroost_pre_acquire(struct rwlock *lock, enum rwlock_acquire_type type);
void wroost_post_acquire(struct rwlock *lock, enum rwlock_acquire_type type);
void wroost_post_release(struct rwlock *lock, enum rwlock_acquire_type type);
/* Separate from switch_in. this runs deferred DPC
 * notifications/IPIs onto the other CPUs */
void wroost_scheduler_switch_in_hook(void);
void wroost_switch_in(struct thread *t);

void wroost_switch_out(struct thread *t);
void wroost_pre_wait(struct thread *curr);
void wroost_post_wait(struct thread *curr);
void wroost_post_migrate_hook(struct thread *t, cpu_id_t from, cpu_id_t to);

size_t wroost_kick(struct wroost_kicks *kicks);
bool wroost_merge_kicks(struct wroost_percpu_data *pcd,
                        struct wroost_kicks *kicks);

struct wroost_kicks cc_warn_unused_result
wroost_post_prio_change_locked(struct thread *t, enum thread_prio_class old,
                               enum thread_prio_class new);

void wroost_init(void);
void wroost_scheduler_init(struct scheduler *sched);
