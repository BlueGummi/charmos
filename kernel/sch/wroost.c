#include <mem/alloc.h>
#include <mem/nofail.h>
#include <sch/sched.h>
#include <sch/wroost.h>
#include <sync/rwlock.h>
#include <thread/thread.h>

#include "internal.h"

struct wroost_globals {
    struct wroost_hash_table hash_table;
};

struct wroost_globals wroost_global;

static size_t next_ctr(const wroost_counter_t *ctrs, size_t total,
                       size_t start) {
    if (start >= total)
        return total;

    size_t ctr_count = (total + WROOST_COUNTER_MAX) >> WROOST_BITS_PER_COUNTER;
    size_t c = start >> WROOST_BITS_PER_COUNTER;
    uint8_t mask = UINT8_MAX
                   << ((start & WROOST_COUNTER_MAX) * WROOST_BITS_PER_COUNTER);

    do {
        uint8_t ctr = ctrs[c] & mask;
        if (ctr != 0) {
            size_t idx = (c << WROOST_BITS_PER_COUNTER) | (ci_ctz(ctr) >> 1);
            return MIN(idx, total);
        }

        mask = UINT8_MAX;
        c++;
    } while (c < ctr_count);

    return total;
}

#define wroost_for_each_set_ctr(ctr_val, i, ctrs, n_bytes)                     \
    for (size_t _tot_##i =                                                     \
             (size_t) (n_bytes) *                                              \
             (BIT_SIZEOF(wroost_counter_t) / WROOST_BITS_PER_COUNTER),         \
                _active_##i = 1;                                               \
         _active_##i && _tot_##i > 0; _active_##i = 0)                         \
        for ((i) = next_ctr((ctrs), _tot_##i, 0);                              \
             (i) < _tot_##i &&                                                 \
             (((ctr_val) =                                                     \
                   (wroost_counter_t) (((ctrs)[(i) >>                          \
                                               WROOST_BITS_PER_COUNTER] >>     \
                                        (((i) & WROOST_COUNTER_MASK) *         \
                                         WROOST_BITS_PER_COUNTER)) &           \
                                       WROOST_COUNTER_MASK)),                  \
              1);                                                              \
             (i) = next_ctr((ctrs), _tot_##i, (i) + 1))

/*
 * Invariants to check:
 *
 * (1) All counters are either all in the exact_entries or not
 * (2) Saturated counters or zero counters have 0 fine_entries
 *
 *
 *
 */
#define wroost_check_enforce(wts) enforce(wroost_state_check(wts))
static bool wroost_state_check(struct wroost_thread_data *wts) {
    cc_ignore(wts);

#ifdef DEBUG_WROOST
    for (int i = 0; i < WROOST_THREAD_COUNTERS; i++) {
        wroost_counter_t ctr = wroost_read_counter(wts, i);
        int found = 0;
        for (int j = 0; j < WROOST_FINE_ENTRY_COUNT; j++) {
            if ((wts->fine_valid & BIT(j)) &&
                wroost_fine_entry_index(&wts->fine_entries[j]) == i)
                found++;
        }

        if (ctr == 0 || ctr == WROOST_COUNTER_MAX) {
            if (found != 0)
                return false;
        } else {
            if (found && found != ctr)
                return false;
        }
    }

#endif

    return true;
}

static void bucket_idx_and_offset(cpu_id_t cpu, size_t *out_idx,
                                  size_t *out_offset) {
    size_t idx = cpu * THREAD_PRIO_CLASS_BIT_COUNT;
    if (out_idx)
        *out_idx = idx / BITMAP_BITS_PER_WORD;

    if (out_offset)
        *out_offset = idx % BITMAP_BITS_PER_WORD;
}

static enum thread_prio_class read_bucket_prio(struct wroost_bucket *bkt,
                                               cpu_id_t cpu) {
    size_t idx, offset;
    bucket_idx_and_offset(cpu, &idx, &offset);
    bitmap_word_t word = atomic_load(&bkt->rq_min_prio[idx]);
    return (word >> offset) & THREAD_PRIO_CLASS_MASK;
}

static enum thread_prio_class update_bucket_prio(struct wroost_bucket *bkt,
                                                 cpu_id_t cpu,
                                                 enum thread_prio_class prio) {
    enum thread_prio_class old = read_bucket_prio(bkt, cpu);
    if (old > prio) {
        size_t idx, offset;
        bucket_idx_and_offset(cpu, &idx, &offset);
        enum thread_prio_class new = (old ^ prio) << offset;
        atomic_fetch_xor(&bkt->rq_min_prio[idx], new);
    }

    return old;
}

static void percpu_inc_id(struct wroost_percpu_data *pcpu, wroost_id_t lock_id,
                          enum thread_prio_class prio) {
    pcpu->rq_bucket_counts[lock_id]++;

    struct wroost_bucket *bkt = &wroost_global.hash_table.buckets[lock_id];
    update_bucket_prio(bkt, pcpu->id, prio);
}

/*
 * We do a trick here. If the 2-bit priority class counter in the bucket is
 * 0b11, this semantically means "urgent is the minimum", but functionally
 * means "no need to reschedule this CPU for a writer->reader boost".
 *
 * This functionality is identical to what we want when a CPU has no threads
 * of a given lock ID, so we stash an URGENT when clearing.
 */
static void percpu_dec_id(struct wroost_percpu_data *pcpu,
                          wroost_id_t lock_id) {
    enforce(pcpu->rq_bucket_counts[lock_id] > 0);
    if (--pcpu->rq_bucket_counts[lock_id] == 0) {
        struct wroost_bucket *bkt = &wroost_global.hash_table.buckets[lock_id];
        size_t idx, offset;
        bucket_idx_and_offset(pcpu->id, &idx, &offset);
        enum thread_prio_class new = THREAD_PRIO_CLASS_URGENT << offset;
        atomic_fetch_or(&bkt->rq_min_prio[idx], new);
    }
}

static void percpu_move_id(struct wroost_percpu_data *src_pcpu,
                           struct wroost_percpu_data *dst_pcpu,
                           wroost_id_t lock_id, enum thread_prio_class prio) {
    /* Both runqueues are locked at this time,
     * as we're in the migration hook */
    enforce(src_pcpu->rq_bucket_counts[lock_id]);

    /* set the destination before we clear the source, because
     * "false positives are harmless, false negatives are harmful" */
    percpu_inc_id(dst_pcpu, lock_id, prio);
    percpu_dec_id(src_pcpu, lock_id);
}

static enum thread_prio_class bucket_ceil_for(size_t bkt_idx,
                                              enum thread_prio_class max_boost,
                                              enum thread_prio_class min) {
    enforce(max_boost < THREAD_PRIO_CLASS_COUNT);

    if (max_boost <= min)
        return min;

    uint64_t bucket_word =
        atomic_load(&wroost_global.hash_table.buckets[bkt_idx].raw_tally);

    if (cc_likely(bucket_word == 0))
        return min;

    union {
        uint16_t tallies[THREAD_PRIO_CLASS_COUNT];
        uint64_t raw;
    } exp = {.raw = bucket_word};

    enum thread_prio_class ceil = min;
    for (int p = max_boost; p > min; p--) {
        if (exp.tallies[p] > 0) {
            ceil = MAX((enum thread_prio_class) p, ceil);
            break;
        }
    }

    return ceil;
}

static enum thread_prio_class fine_entry_ceil(struct wroost_thread_data *wts,
                                              struct wroost_fine_entry *fe,
                                              enum thread_prio_class min) {
    size_t idx = wroost_fine_entry_index(fe) * WROOST_COARSE_FACTOR +
                 (fe->lock_id & WROOST_FINE_OFFSET_MASK);
    return bucket_ceil_for(idx, wts->max_boost, min);
}

enum thread_prio_class wroost_compute_ceil(struct wroost_thread_data *wts,
                                           enum thread_prio_class min) {
    wroost_check_enforce(wts);

    int checked_fine_entries = 0;
    int total_valid_fine = ci_popcount(wts->fine_valid);
    enum thread_prio_class ceil = min;

    wroost_counter_t val;
    wroost_id_t i;
    wroost_for_each_set_ctr(val, i, wts->counters,
                            WROOST_THREAD_COUNTER_BYTES) {

        bool was_fine = false;
        if (checked_fine_entries != total_valid_fine) {
            for (int j = 0; j < WROOST_FINE_ENTRY_COUNT; j++) {
                if (!(wts->fine_valid & BIT(j)))
                    continue;

                struct wroost_fine_entry *fe = &wts->fine_entries[j];
                if (wroost_fine_entry_index(fe) == i) {
                    ceil = MAX(fine_entry_ceil(wts, fe, ceil), ceil);
                    was_fine = true;
                    checked_fine_entries++;
                }
            }
        }

        if (was_fine)
            continue;

        size_t idx = (size_t) i * WROOST_COARSE_FACTOR;
        for (size_t j = idx; j < idx + WROOST_COARSE_FACTOR; j++)
            ceil = MAX(bucket_ceil_for(j, wts->max_boost, ceil), ceil);
    }

    return ceil;
}

void wroost_post_read_acquire(struct rwlock *lock,
                              enum thread_prio_class lock_max_ceil) {
    struct thread *curr = thread_get_current();
    wroost_id_t id = wroost_hash(lock);
    uint8_t c_idx = id / WROOST_COARSE_FACTOR;
    uint8_t offset = id % WROOST_COARSE_FACTOR;
    enforce((offset & WROOST_FINE_OFFSET_MASK) == offset);
    struct wroost_thread_data *wts = &curr->wroost_data;

    /* We're touching per-thread that the scheduler cares about,
     * so let's prevent preemption so it always sees a valid state */
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    wroost_check_enforce(wts);

    if (wts->read_nesting_depth == 0 || lock_max_ceil > wts->max_boost)
        wts->max_boost = lock_max_ceil;

    wts->read_nesting_depth++;

    wroost_counter_t prev_val;
    wroost_counter_t new_val = wroost_inc_counter(wts, c_idx, &prev_val);

    if (prev_val == 0) {
        /* First lock in this index, try to get an entry */
        struct wroost_fine_entry *fe = wroost_alloc_fine_entry(wts);
        if (fe)
            fe->lock_id = (c_idx << WROOST_FINE_INDEX_SHIFT) | offset;

    } else if (new_val == WROOST_COUNTER_MAX) {
        /* Evict entries */
        wroost_evict_fine_entries(wts, c_idx);
    } else {
        /* second lock, allocate a fine entry only if the first one exists,
         * evicting on allocation failure */
        if (wroost_get_fine_entry(wts, c_idx) != NULL) {
            struct wroost_fine_entry *fe = wroost_alloc_fine_entry(wts);
            if (fe) {
                fe->lock_id = (c_idx << WROOST_FINE_INDEX_SHIFT) | offset;
            } else {
                wroost_evict_fine_entries(wts, c_idx);
            }
        }
    }

    wroost_check_enforce(wts);
    irql_lower(irql);
}

void wroost_post_read_release(struct rwlock *lock) {
    struct thread *curr = thread_get_current();
    struct wroost_thread_data *wts = &curr->wroost_data;

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    wroost_check_enforce(wts);
    enforce(wts->read_nesting_depth > 0);

    if (cc_likely(--wts->read_nesting_depth == 0)) {
        for (int i = 0; i < WROOST_THREAD_COUNTER_BYTES; i++)
            wts->counters[i] = 0;

        wts->fine_valid = 0;
        wts->max_boost = 0;
    } else {
        wroost_id_t id = wroost_hash(lock);
        uint8_t c_idx = id / WROOST_COARSE_FACTOR;
        uint8_t offset = id % WROOST_COARSE_FACTOR;

        wroost_counter_t prev_val = wroost_read_counter(wts, c_idx);
        wroost_dec_counter(wts, c_idx);

        if (prev_val < WROOST_COUNTER_MAX) {
            for (int i = 0; i < WROOST_FINE_ENTRY_COUNT; i++) {
                struct wroost_fine_entry *wfe = &wts->fine_entries[i];
                uint8_t wfe_idx = wroost_fine_entry_index(wfe);
                uint8_t wfe_offset = wfe->lock_id & WROOST_FINE_OFFSET_MASK;
                if ((wts->fine_valid & BIT(i)) && wfe_idx == c_idx &&
                    wfe_offset == offset) {
                    wroost_clear_fine_entry(wts, i);
                    break;
                }
            }
        }
    }

    wroost_check_enforce(wts);
    irql_lower(irql);
}

static bool percpu_boost_kick(cpu_id_t target) {
    dpc_enqueue_on_cpu(target,
                       &global.schedulers[target]->wroost_data->boost_dpc);
}

void wroost_writer_block(struct rwlock *lock,
                         enum thread_prio_class writer_class) {
    wroost_id_t bkt_idx = wroost_hash(lock);
    struct wroost_bucket *bkt = &wroost_global.hash_table.buckets[bkt_idx];
    size_t offset = (size_t) writer_class * BIT_SIZEOF(uint16_t);
    uint64_t inc = BIT(offset);
    uint64_t prev_raw = atomic_fetch_add(&bkt->raw_tally, inc);

    atomic_thread_fence(mo_seq_cst);

    /* 0 -> 1 transition, we handle IPI'ing and whatnot */
    uint16_t prev_class_cnt = (prev_raw >> offset) & UINT16_MAX;

    if (prev_class_cnt == 0) {
        cpu_id_t checked = 0;
        for (size_t bm_word_idx = 0; checked < global.core_count;
             bm_word_idx++) {
            bitmap_word_t word = atomic_load(&bkt->rq_min_prio[bm_word_idx]);

            for (int shift = 0;
                 shift < BITMAP_BITS_PER_WORD && checked < global.core_count;
                 shift += THREAD_PRIO_CLASS_BIT_COUNT) {
                enum thread_prio_class prio =
                    (word >> shift) & THREAD_PRIO_CLASS_MASK;

                if (prio < writer_class)
                    percpu_boost_kick(checked);

                checked++;
            }
        }
    }
}

/* There's a balance to strike with the scheduler runqueue lock granularity.
 *
 * If we lock around the WHOLE DPC, then writers can stall waiting
 * for us to complete it if we are in the middle of it.
 *
 * This is because the IPI would wait for us to exit our IRQ disabled
 * critical section, which would be bounded by the time we hold
 * the lock for. In contrast, holding it for too little is also
 * not great since the lock paths get noisy.
 *
 * We break it to the per-bitmap-pass granularity.
 */
static void process_pending_bitmap(struct scheduler *self) {
    struct wroost_percpu_data *pcd = self->wroost_data;

    enum irql irql = spin_lock_high(&self->lock);

    for (size_t w = 0; w < BITMAP_WORDS(WROOST_BUCKET_COUNT); w++) {
        uint64_t pending = atomic_xchg_acq_rel(&pcd->pending_bitmap[w], 0);

        while (pending) {
            uint8_t bit = cw_ctz(pending);
            pending &= pending - 1;
            wroost_id_t id = (w << WROOST_PENDING_BITMAP_WORD_SHIFT) | bit;
        }
    }

    spin_unlock(&self->lock, irql);
}

/* This DPC runs on the reader-side CPU and is
 * responsible for doing all of the boosting. */
static void wroost_dpc(struct dpc_context *dc, void *a, void *b) {
    cc_ignore(dc, a, b);
    struct scheduler *self = smp_core_scheduler();
    while (true) {
        process_pending_bitmap(self);

        /* If a writer came about, this CAS fails and we go again */
        enum wroost_state want = WROOST_DPC_ACTIVE;

        if (cc_likely(atomic_cas_strong(&self->wroost_data->state, &want,
                                        WROOST_DPC_IDLE)))
            break;
    }
}

void wroost_switch_out(struct wroost_percpu_data *pcpu, struct thread *out) {}

void wroost_migrate_hook(struct wroost_percpu_data *from,
                         struct wroost_percpu_data *to, struct thread *thread) {
}

void wroost_scheduler_init(struct scheduler *sched) {
    sched->wroost_data = nofail_vnew(sched->wroost_data, ALLOC_ZERO);

    struct wroost_percpu_data *pcd = sched->wroost_data;
    dpc_init(&pcd->boost_dpc, wroost_dpc, NULL, NULL);
    pcd->id = sched->core_id;
}
