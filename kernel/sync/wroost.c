#include <sch/sched.h>
#include <sync/rwlock.h>
#include <sync/wroost.h>
#include <thread/thread.h>

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
static bool wroost_state_check(struct wroost_thread_state *wts) {
    cc_ignore(wts);
#ifdef DEBUG_WROOST
    for (int i = 0; i < WROOST_THREAD_COUNTERS; i++) {
        wroost_counter_t ctr = wroost_read_counter(wts, i);
        int found = 0;
        for (int j = 0; j < WROOST_FINE_ENTRY_COUNT; j++) {
            if ((wts->fine_valid & (1U << j)) &&
                wts->fine_entries[j].index == i)
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

static void percpu_inc_id(struct wroost_percpu *pcpu, wroost_id_t lock_id,
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
static void percpu_dec_id(struct wroost_percpu *pcpu, wroost_id_t lock_id) {
    enforce(pcpu->rq_bucket_counts[lock_id] > 0);
    if (--pcpu->rq_bucket_counts[lock_id] == 0) {
        struct wroost_bucket *bkt = &wroost_global.hash_table.buckets[lock_id];
        size_t idx, offset;
        bucket_idx_and_offset(pcpu->id, &idx, &offset);
        enum thread_prio_class new = (THREAD_PRIO_CLASS_URGENT) << offset;
        atomic_fetch_or(&bkt->rq_min_prio[idx], new);
    }
}

static void percpu_move_id(struct wroost_percpu *src_pcpu,
                           struct wroost_percpu *dst_pcpu, wroost_id_t lock_id,
                           enum thread_prio_class prio) {
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

static enum thread_prio_class fine_entry_ceil(struct wroost_thread_state *wts,
                                              struct wroost_fine_entry *fe,
                                              enum thread_prio_class min) {
    size_t idx = (size_t) fe->index * WROOST_COARSE_FACTOR + fe->offset;
    return bucket_ceil_for(idx, wts->max_boost, min);
}

enum thread_prio_class wroost_compute_ceil(struct wroost_thread_state *wts,
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
                if (fe->index == i) {
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
    struct wroost_thread_state *wts = &curr->wroost_state;

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
        if (fe) {
            fe->index = c_idx;
            fe->offset = offset;
        }
    } else if (new_val == WROOST_COUNTER_MAX) {
        /* Evict entries */
        wroost_evict_fine_entries(wts, c_idx);
    } else {
        /* second lock, allocate a fine entry only if the first one exists,
         * evicting on allocation failure */
        if (wroost_get_fine_entry(wts, c_idx) != NULL) {
            struct wroost_fine_entry *fe = wroost_alloc_fine_entry(wts);
            if (fe) {
                fe->index = c_idx;
                fe->offset = offset;
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
    struct wroost_thread_state *wts = &curr->wroost_state;

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
                if ((wts->fine_valid & BIT(i)) &&
                    wts->fine_entries[i].index == c_idx &&
                    wts->fine_entries[i].offset == offset) {
                    wroost_clear_fine_entry(wts, i);
                    break;
                }
            }
        }
    }

    wroost_check_enforce(wts);
    irql_lower(irql);
}

void wroost_writer_bump(struct rwlock *lock,
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

                if (prio < writer_class) {
                    /* TODO: Kick */
                }

                checked++;
            }
        }
    }
}

void wroost_switch_out(struct wroost_percpu *pcpu, struct thread *out) {}

void wroost_switch_in(struct wroost_percpu *pcpu, struct thread *in) {}

void wroost_migrate_hook(struct wroost_percpu *from, struct wroost_percpu *to,
                         struct thread *thread) {}
