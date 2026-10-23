#include <mem/alloc.h>
#include <mem/nofail.h>
#include <sch/sched.h>
#include <sch/wroost.h>
#include <sync/rwlock.h>
#include <sync/turnstile.h>
#include <thread/thread.h>

#include "atomic.h"
#include "internal.h"
#include "sync/rwlock_internal.h"

/* The way that fine entries work:
 *
 * Coarse entries are fixed, however, fine entries are dynamic based on
 * hash_table.bucket_count. In order to derive a fine entry index shift,
 * we figure out the number of bits that a coarse entry cannot cover.
 *
 * This is done through a
 * BITS_NEEDED((bucket_count/WROOST_THREAD_COUNTERS)-1), as
 * it represents the number of bits the thread counters miss.
 */
struct wroost_globals {
    struct wroost_hash_table hash_table;
    size_t fine_index_bits;
    size_t fine_offset_mask;
};

struct wroost_globals wroost_global = {0};

#define WROOST_KICK_UPDATE_PENDING (1)
#define WROOST_KICK_SEND_DPCS (1 << 1)
#define WROOST_COARSE_FACTOR                                                   \
    (wroost_global.hash_table.bucket_count / WROOST_THREAD_COUNTERS)

static size_t wroost_kick_cpus(struct cpu_mask *mask, wroost_id_t lock_id,
                               int options);
static bool percpu_boost_kick(cpu_id_t target, wroost_id_t lock_id,
                              int options);

static inline size_t bucket_count(void) {
    return wroost_global.hash_table.bucket_count;
}

/* Counters are two bits. Index 0 is bit 0, index 1 is bit 2, index 2 is bit 4, etc. */
static inline uint8_t wroost_get_counter(struct wroost_thread_data *wtd,
                                         uint8_t index, uint8_t *byte_idx_out,
                                         uint8_t *byte_offset_out) {
    enforce(index < WROOST_THREAD_COUNTERS);
    uint8_t bits_per_ctr_word = BIT_SIZEOF(wroost_counter_t);
    size_t bit_index = index * WROOST_BITS_PER_COUNTER;

    if (byte_idx_out)
        *byte_idx_out = bit_index / bits_per_ctr_word;

    if (byte_offset_out)
        *byte_offset_out = bit_index % bits_per_ctr_word;

    return wtd->counters[bit_index / bits_per_ctr_word];
}

static inline wroost_counter_t
wroost_read_counter(struct wroost_thread_data *wtd, uint8_t ctr_idx) {
    uint8_t idx, offset;
    wroost_counter_t ctr = wroost_get_counter(wtd, ctr_idx, &idx, &offset);
    return (ctr >> offset) & WROOST_COUNTER_MASK;
}

static inline void wroost_write_counter(struct wroost_thread_data *wtd,
                                        uint8_t ctr_idx, uint8_t new_ctr_val) {
    enforce((new_ctr_val & WROOST_COUNTER_MASK) == new_ctr_val);

    uint8_t idx, offset;
    wroost_counter_t ctr = wroost_get_counter(wtd, ctr_idx, &idx, &offset);
    wroost_counter_t cleared = ctr & ~(WROOST_COUNTER_MASK << offset);
    wroost_counter_t new = (new_ctr_val << offset) | cleared;
    wtd->counters[idx] = new;
}

static inline uint8_t wroost_inc_counter(struct wroost_thread_data *wtd,
                                         uint8_t ctr_idx,
                                         wroost_counter_t *ctr_val) {
    if ((*ctr_val = wroost_read_counter(wtd, ctr_idx)) == WROOST_COUNTER_MAX)
        return WROOST_COUNTER_MAX;

    wroost_write_counter(wtd, ctr_idx, *ctr_val + 1);
    return *ctr_val + 1;
}

static inline uint8_t wroost_dec_counter(struct wroost_thread_data *wtd,
                                         uint8_t ctr_idx) {
    wroost_counter_t ctr_val;
    if ((ctr_val = wroost_read_counter(wtd, ctr_idx)) == WROOST_COUNTER_MAX)
        return WROOST_COUNTER_MAX;

    enforce(ctr_val > 0);
    wroost_write_counter(wtd, ctr_idx, ctr_val - 1);
    return ctr_val - 1;
}

static inline uint16_t wroost_read_tally(struct wroost_bucket *bkt,
                                         enum thread_prio_class class) {
    enforce(class < THREAD_PRIO_CLASS_COUNT);
    uint64_t ret = atomic_load_acq(&bkt->raw_tally);
    return (ret >> ((size_t) class * BIT_SIZEOF(uint16_t))) & UINT16_MAX;
}

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

static inline wroost_id_t wroost_hash(struct rwlock *lock) {
    return hash_pointer(lock) % wroost_global.hash_table.bucket_count;
}

static inline uint8_t wroost_fine_entry_index(struct wroost_fine_entry *ent) {
    return ent->lock_id >> wroost_global.fine_index_bits;
}

static inline struct wroost_fine_entry *
wroost_get_fine_entry(struct wroost_thread_data *wtd, uint8_t index) {
    enforce(index < WROOST_THREAD_COUNTERS);
    for (int i = 0; i < WROOST_FINE_ENTRY_COUNT; i++) {
        if ((wtd->fine_valid & BIT(i)) &&
            wroost_fine_entry_index(&wtd->fine_entries[i]) == index)
            return &wtd->fine_entries[i];
    }

    return NULL;
}

static inline void wroost_clear_fine_entry(struct wroost_thread_data *wtd,
                                           int slot) {
    enforce(slot >= 0 && slot < WROOST_FINE_ENTRY_COUNT);
    wtd->fine_entries[slot].lock_id = 0;
    wtd->fine_valid &= ~BIT(slot);
}

static inline void wroost_evict_fine_entries(struct wroost_thread_data *wtd,
                                             uint8_t index) {
    for (int i = 0; i < WROOST_FINE_ENTRY_COUNT; i++) {
        if ((wtd->fine_valid & BIT(i)) &&
            wroost_fine_entry_index(&wtd->fine_entries[i]) == index)
            wroost_clear_fine_entry(wtd, i);
    }
}

static inline struct wroost_fine_entry *
wroost_alloc_fine_entry(struct wroost_thread_data *wtd) {
    /* Notable edge case here:
     *
     * the index can be 0, so we track slot liveness explicitly via fine_valid
     * instead of relying on index == 0 meaning 'free'.
     */
    uint8_t free_mask = (~wtd->fine_valid) & WROOST_FINE_VALID_MASK;
    if (free_mask == 0)
        return NULL;

    int i = ci_ctz(free_mask);
    wtd->fine_valid |= BIT(i);
    return &wtd->fine_entries[i];
}

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
 * (3) There are no duplicate fine entries
 */
#define wroost_check_enforce(wtd) enforce(wroost_state_check(wtd))
static bool wroost_state_check(struct wroost_thread_data *wtd) {
    cc_ignore(wtd);

#ifdef DEBUG_WROOST
    for (int i = 0; i < WROOST_FINE_ENTRY_COUNT; i++) {
        if (wtd->fine_valid & BIT(i)) {
            struct wroost_fine_entry *ent = &wtd->fine_entries[i];
            for (int j = 0; j < WROOST_FINE_ENTRY_COUNT; j++) {
                if (i != j && (wtd->fine_valid & BIT(j))) {
                    if (ent->lock_id == wtd->fine_entries[j].lock_id)
                        return false;
                }
            }
        }
    }

    for (int i = 0; i < WROOST_THREAD_COUNTERS; i++) {
        wroost_counter_t ctr = wroost_read_counter(wtd, i);
        int found = 0;
        for (int j = 0; j < WROOST_FINE_ENTRY_COUNT; j++) {
            if ((wtd->fine_valid & BIT(j)) &&
                wroost_fine_entry_index(&wtd->fine_entries[j]) == i)
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

static void percpu_inc_id(struct wroost_percpu_data *pcpu, wroost_id_t lock_id,
                          enum thread_prio_class prio) {
    pcpu->rq_bucket_counts[lock_id]++;

    struct wroost_bucket *bkt = &wroost_global.hash_table.buckets[lock_id];

    enum thread_prio_class old = read_bucket_prio(bkt, pcpu->id);
    if (old > prio) {
        size_t idx, offset;
        bucket_idx_and_offset(pcpu->id, &idx, &offset);
        enum thread_prio_class new = (old ^ prio) << offset;
        atomic_fetch_xor(&bkt->rq_min_prio[idx], new);
    }
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

static enum thread_prio_class ceil_for(wroost_id_t lock_id,
                                       enum thread_prio_class max_boost,
                                       enum thread_prio_class min) {
    enforce(max_boost < THREAD_PRIO_CLASS_COUNT);

    /* This is for readers. If the max boost is less than the minimum (base)
     * thread priority, it should've panicked on lock entry. */
    enforce(min <= max_boost);

    uint64_t bucket_word =
        atomic_load(&wroost_global.hash_table.buckets[lock_id].raw_tally);

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

/* Use the raw recomputation because that avoids sending out DPCs,
 * which can deadlock us. Writers handle DPCs and tallies in their switch out
 *
 * TODO: Re-enqueue DPCs */
static void boost_thread_to(struct thread *t, enum thread_prio_class prio) {
    struct wroost_thread_data *wtd = &t->wroost_data;
    enforce(prio <= wtd->max_boost);

    spin_guard_high(&t->pi_lock);

    if (!wtd->contributed_floor) {
        wtd->floor = prio;
        wtd->contributed_floor = true;
        thread_class_floor_inc_locked(t, prio);
    } else if (prio > wtd->floor) {
        thread_class_floor_inc_locked(t, prio);
        thread_class_floor_dec_locked(t, wtd->floor);
        wtd->floor = prio;
    }

    thread_recompute_prio_class_raw_locked(t, NULL, NULL);
}

bool wroost_merge_kicks(struct wroost_percpu_data *pcd,
                        struct wroost_kicks *kicks) {
    bool bother_self = false;
    if (kicks->need_kicks) {
        cpu_mask_or(&pcd->in_dpc_deferred_cpus, &kicks->cpus);
        bitmap_set(pcd->in_dpc_deferred_ids, kicks->lock_id);

        /* We don't re-enqueue the DPC on ourselves, that's unnecessary
         * since the DPC itself can simply loop again */
        cpu_mask_clear(&pcd->in_dpc_deferred_cpus, pcd->id);

        if (cpu_mask_test(&kicks->cpus, pcd->id)) {
            bother_self = true;

            struct cpu_mask self_mask = CPU_MASK_INIT;

            cpu_mask_set(&self_mask, pcd->id);

            /* Just pend the update on the current CPU */
            wroost_kick_cpus(&self_mask, kicks->lock_id,
                             WROOST_KICK_UPDATE_PENDING);
        }
    }

    return bother_self;
}

static struct wroost_kicks boost_thread_in_dpc(struct scheduler *sched,
                                               struct thread *t,
                                               enum thread_prio_class prio) {
    SPINLOCK_ASSERT_HELD(&sched->lock);
    struct wroost_percpu_data *pcd = sched->wroost_data;

    struct wroost_thread_data *wtd = &t->wroost_data;
    enforce(prio <= wtd->max_boost);

    struct wroost_kicks kicks = {0};

    enum irql irql = spin_lock_high(&t->pi_lock);
    enum thread_prio_class old, new;

#ifdef DEBUG_WROOST
    /* Should effectively be a no-op */
    thread_recompute_prio_class_raw_locked(t, &old, &new);
    enforce(old == new, "likely stale priority floor");
#endif

    if (!wtd->contributed_floor) {
        wtd->floor = prio;
        wtd->contributed_floor = true;
        thread_class_floor_inc_locked(t, prio);
    } else if (prio > wtd->floor) {
        thread_class_floor_inc_locked(t, prio);
        thread_class_floor_dec_locked(t, wtd->floor);
        wtd->floor = prio;
    }

    thread_recompute_prio_class_dry_locked(t, &old, &new);

    kicks = wroost_post_prio_change_locked(t, old, new);
    wroost_merge_kicks(pcd, &kicks);

    /* If this happens, this MUST be because of us.
     *
     * NOTE: scheduler_remove_thread_locked does not change the
     * perceived_prio_class, so a removal and re-insertion is fine */
    if (old != new) {
        if (thread_get_state(t) == THREAD_STATE_READY)
            scheduler_remove_thread_locked(sched, t);

        t->perceived_prio_class = new;

        if (thread_get_state(t) == THREAD_STATE_READY)
            scheduler_add_thread_locked(sched, t);
    }

    spin_unlock(&t->pi_lock, irql);

    return kicks;
}

static void unboost_thread(struct thread *t) {
    struct wroost_thread_data *wtd = &t->wroost_data;
    spin_guard_high(&t->pi_lock);

    if (wtd->contributed_floor)
        thread_class_floor_dec_locked(t, wtd->floor);

    wtd->floor = 0;
    wtd->contributed_floor = false;

    /* Unboost is fine to ignore the kicks from a boost */
    cc_ignore(thread_recompute_prio_class_locked(t));
}

static enum thread_prio_class ceil_for_thread(wroost_id_t id,
                                              struct thread *t) {
    return ceil_for(id, t->wroost_data.max_boost, t->base_prio_class);
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
 *
 * Readers that are also writers on other locks are a special case.
 *
 * There need not be another list that checks them, but rather,
 * they will simply end up on either thread_map or multi_lock_readers.
 *
 * For such threads, the DPC will need to use a CPU mask that
 */
static bool process_pending_word(struct scheduler *self, size_t w_idx,
                                 bitmap_word_t pending,
                                 struct wroost_process_result *result) {
    struct thread *iter;
    struct wroost_percpu_data *pcd = self->wroost_data;

    while (pending) {
        uint8_t bit = cw_ctz(pending);
        pending &= pending - 1;

        wroost_id_t id = (w_idx * BIT_SIZEOF(bitmap_word_t)) | bit;
        wroost_id_t coarse = id / WROOST_COARSE_FACTOR;
        struct wroost_kicks kicks = {0};

        enforce(id < bucket_count());
        enforce(coarse < WROOST_THREAD_COUNTERS);

        hlist_for_each_entry(iter, &pcd->thread_map[coarse],
                             wroost_data.map_node) {
#ifdef DEBUG_WROOST
            enforce(!atomic_load_acq(&iter->blocked_ts));
#endif
            enum thread_prio_class new = ceil_for_thread(id, iter);
            kicks = boost_thread_in_dpc(self, iter, new);
            if (cpu_mask_test(&kicks.cpus, self->core_id))
                return (result->restart = true);
        }

        hlist_for_each_entry(iter, &pcd->multi_lock_readers,
                             wroost_data.map_node) {
#ifdef DEBUG_WROOST
            enforce(!atomic_load_acq(&iter->blocked_ts));
#endif
            struct wroost_thread_data *wtd = &iter->wroost_data;

            for (int i = 0; i < WROOST_FINE_ENTRY_COUNT; i++) {
                struct wroost_fine_entry *ent = &wtd->fine_entries[i];

                /* If we find it in a fine entry, we know this is the only
                 * occurrence because the fine entries
                 * cannot contain duplicates */
                if ((wtd->fine_valid & BIT(i)) && ent->lock_id == id) {
                    enum thread_prio_class new = ceil_for_thread(id, iter);
                    kicks = boost_thread_in_dpc(self, iter, new);
                    if (cpu_mask_test(&kicks.cpus, self->core_id))
                        return (result->restart = true);

                    goto next;
                }
            }

            wroost_counter_t cc_unused val;
            wroost_id_t i;
            wroost_for_each_set_ctr(val, i, wtd->counters,
                                    WROOST_THREAD_COUNTERS) {
                if (i != coarse)
                    continue;

                wroost_id_t base = i * WROOST_COARSE_FACTOR;
                enum thread_prio_class to = ceil_for_thread(base, iter);

                /* We start at one because boost_to already checked 0 */
                for (wroost_id_t j = 1; j < WROOST_COARSE_FACTOR; j++) {
                    wroost_id_t lock_id = base + j;
                    to = MAX(to, ceil_for_thread(lock_id, iter));
                }

                kicks = boost_thread_in_dpc(self, iter, to);
                if (cpu_mask_test(&kicks.cpus, self->core_id))
                    return (result->restart = true);
            }

        next:
            continue;
        }
    }

    return false;
}

static bool process_pending_word_ts(struct scheduler *self, size_t w_idx,
                                    bitmap_word_t pending,
                                    struct wroost_process_result *result) {
    struct thread *iter;
    struct wroost_percpu_data *pcd = self->wroost_data;

    while (pending) {
        uint8_t bit = cw_ctz(pending);
        pending &= pending - 1;

        wroost_id_t id = (w_idx * BIT_SIZEOF(bitmap_word_t)) | bit;
        wroost_id_t coarse = id / WROOST_COARSE_FACTOR;

        struct wroost_kicks kicks = {0};

        hlist_for_each_entry(iter, &pcd->blocked_ts_readers,
                             wroost_data.map_node) {
            struct wroost_thread_data *wtd = &iter->wroost_data;

            for (int i = 0; i < WROOST_FINE_ENTRY_COUNT; i++) {
                struct wroost_fine_entry *ent = &wtd->fine_entries[i];

                if ((wtd->fine_valid & BIT(i)) && ent->lock_id == id) {
                    enum thread_prio_class new = ceil_for_thread(id, iter);
                    kicks = boost_thread_in_dpc(self, iter, new);
                    if (cpu_mask_test(&kicks.cpus, self->core_id))
                        return (result->restart = true);

                    if (turnstile_propagate_wroost(pcd, iter))
                        return (result->restart = true);

                    goto next;
                }
            }

            wroost_counter_t cc_unused val;
            wroost_id_t i;
            wroost_for_each_set_ctr(val, i, wtd->counters,
                                    WROOST_THREAD_COUNTERS) {
                if (i != coarse)
                    continue;

                wroost_id_t base = i * WROOST_COARSE_FACTOR;
                enum thread_prio_class to = ceil_for_thread(base, iter);

                for (wroost_id_t j = 1; j < WROOST_COARSE_FACTOR; j++) {
                    wroost_id_t lock_id = base + j;
                    to = MAX(to, ceil_for_thread(lock_id, iter));
                }

                kicks = boost_thread_in_dpc(self, iter, to);
                if (cpu_mask_test(&kicks.cpus, self->core_id))
                    return (result->restart = true);

                if (turnstile_propagate_wroost(pcd, iter))
                    return (result->restart = true);
            }

        next:
            continue;
        }
    }

    return false;
}

static void process_pending_bitmap(struct scheduler *self,
                                   struct wroost_process_result *result) {
    enforce(irql_get() == IRQL_DISPATCH_LEVEL);
    enforce(!result->restart);

    struct wroost_percpu_data *pcd = self->wroost_data;
    bitmap_zero(pcd->in_dpc_deferred_ids, bucket_count());
    cpu_mask_clear_all(&pcd->in_dpc_deferred_cpus);

    enum irql wirql = spin_lock_high(&self->wroost_lock);
    enum irql sirql = spin_lock_high(&self->lock);

    for (size_t w = 0; w < BITMAP_WORDS(bucket_count()); w++) {
        bitmap_word_t pending = atomic_xchg_acq_rel(&pcd->pending_bitmap[w], 0);
        bitmap_word_t pending_prior = pending;

        pending |= result->seen_data[w];

        result->seen_data[w] = pending_prior;

        if (process_pending_word(self, w, pending, result))
            goto out_inner;
    }

    spin_unlock(&self->lock, sirql);

    /* We have to re-check and iterate again under ONLY the wroost_lock.
     *
     * This part is for boosting blocked_ts_readers, which needs to
     * propagate the inheritance up the chain, and thus, requires that
     * no sched->lock be held (only wroost_lock to protect this state) */
    for (size_t w = 0; w < BITMAP_WORDS(bucket_count()); w++) {
        bitmap_word_t new_word =
            atomic_xchg_acq_rel(&pcd->pending_bitmap[w], 0);
        bitmap_word_t pending = new_word | result->seen_data[w];

        /* Add the new words to the set here. This is because on
         * a retry, we start from the beginning again, iterating
         * over non-blocked threads, so we need to preserve those */
        result->seen_data[w] |= new_word;

        if (process_pending_word_ts(self, w, pending, result))
            goto out_outer;
    }

    spin_unlock(&self->wroost_lock, wirql);
    return;

out_inner:
    spin_unlock(&self->lock, sirql);

out_outer:
    spin_unlock(&self->wroost_lock, wirql);
}

static void kick_from_dpc(struct wroost_percpu_data *pcd) {
    /* boost_thread_in_dpc enforces that this doesn't happen. We don't
     * send the DPC on ourselves since we goto restart */
    enforce(!cpu_mask_test(&pcd->in_dpc_deferred_cpus, pcd->id));

    size_t bit;
    bitmap_for_each_bit_set(bit, pcd->in_dpc_deferred_ids,
                            wroost_global.hash_table.bucket_count)
        wroost_kick_cpus(&pcd->in_dpc_deferred_cpus, bit,
                         WROOST_KICK_UPDATE_PENDING | WROOST_KICK_SEND_DPCS);
}

/* This DPC runs on the reader-side CPU and is
 * responsible for doing all of the boosting of READY threads.
 *
 * We don't boost a RUNNING thread here though,
 * that happens on switch out */
static void wroost_dpc(struct dpc_context *dc, void *a, void *b) {
    cc_ignore(dc, a, b);
    struct scheduler *self = smp_core_scheduler();
    struct wroost_percpu_data *pcd = self->wroost_data;

    while (true) {
        atomic_fetch_and(&pcd->state, ~WROOST_DPC_PENDING);

        struct wroost_process_result *result = &pcd->process_result;
        bitmap_zero(result->seen_data, bucket_count());

        /* Every time `kicks` happens to have THIS CPU in its mask,
         * we have to re-scan. This won't infinitely loop (hopefully) because
         * the mask only becomes set upon a priority change,
         * and eventually all priority changes will have completed.
         */
        do {
            result->restart = false;
            process_pending_bitmap(self, result);
            kick_from_dpc(pcd);
        } while (result->restart);

        /* If a writer came about, this CAS fails and we go again */
        enum wroost_state want = WROOST_DPC_ACTIVE;

        if (cc_likely(atomic_cas_strong(&self->wroost_data->state, &want,
                                        WROOST_DPC_IDLE)))
            break;
    }
}

void wroost_pre_acquire(struct rwlock *lock, enum rwlock_acquire_type type) {
    wroost_id_t hash = wroost_hash(lock);
    struct thread *curr = thread_get_current();
    struct wroost_thread_data *wtd = &curr->wroost_data;

    enforce(wtd->writer_state == WROOST_WRITER_NONE);

    /* Nothing for readers to do at the moment */
    if (type == RWLOCK_WRITE) {
        wtd->want_lock_id = hash;
        wtd->writer_state = WROOST_WRITER_WANT;
    }
}

static void post_read_acquire(struct rwlock *lock,
                              enum thread_prio_class lock_max_ceil) {
    struct thread *curr = thread_get_current();
    wroost_id_t id = wroost_hash(lock);
    uint16_t c_idx = id / WROOST_COARSE_FACTOR;
    uint16_t offset = id % WROOST_COARSE_FACTOR;
    enforce((offset & wroost_global.fine_offset_mask) == offset);
    struct wroost_thread_data *wtd = &curr->wroost_data;

    /* We're touching per-thread that the scheduler cares about,
     * so let's prevent preemption so it always sees a valid state */
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    wroost_check_enforce(wtd);

    if (wtd->read_nesting_depth == 0 || lock_max_ceil > wtd->max_boost)
        wtd->max_boost = lock_max_ceil;

    wtd->read_nesting_depth++;

    wroost_counter_t prev_val;
    wroost_counter_t new_val = wroost_inc_counter(wtd, c_idx, &prev_val);
    enforce(((c_idx << wroost_global.fine_index_bits) | offset) == id);

    if (prev_val == 0) {
        /* First lock in this index, try to get an entry */
        struct wroost_fine_entry *fe = wroost_alloc_fine_entry(wtd);
        if (fe)
            fe->lock_id = id;

    } else if (new_val == WROOST_COUNTER_MAX) {
        /* Evict entries */
        wroost_evict_fine_entries(wtd, c_idx);
    } else {
        /* second lock, allocate a fine entry only if the first one exists,
         * evicting on allocation failure */
        struct wroost_fine_entry *old;
        if ((old = wroost_get_fine_entry(wtd, c_idx)) != NULL) {
            /* We cannot end up with a duplicate ID in the fine
             * entries, this would break the unlock path */
            if (old->lock_id == id) {
                wroost_evict_fine_entries(wtd, c_idx);
            } else {
                struct wroost_fine_entry *fe = wroost_alloc_fine_entry(wtd);
                if (fe) {
                    fe->lock_id = id;
                } else {
                    wroost_evict_fine_entries(wtd, c_idx);
                }
            }
        }
    }

    wroost_check_enforce(wtd);
    irql_lower(irql);
}

void wroost_post_acquire(struct rwlock *lock, enum rwlock_acquire_type type) {
    struct thread *curr = thread_get_current();
    struct wroost_thread_data *wtd = &curr->wroost_data;

    if (type == RWLOCK_READ) {
        enum thread_prio_class ceil =
            RWLOCK_GET_MAX_PRIO(RWLOCK_READ_LOCK_WORD(lock));
        post_read_acquire(lock, ceil);
    } else {
        /* We cannot have been mid-wait and somehow got the lock.
         * We either wanted the lock, didn't block, or blocked,
         * in which case it would transition to BUMP */
        enforce(wtd->writer_state != WROOST_WRITER_WAIT);
        enforce(wtd->writer_state != WROOST_WRITER_NONE);

        /* We incremented boost counters. Acquire the pi_lock so that
         * updates to our perceived priority synchronize and we decrement
         * properly and update the state */
        if (wtd->writer_state == WROOST_WRITER_BUMP) {
            enum irql irql = spin_lock_high(&curr->pi_lock);

            enum thread_prio_class bumped = curr->perceived_prio_class;

            wroost_id_t lock_id = wtd->want_lock_id;
            struct wroost_bucket *bkt =
                &wroost_global.hash_table.buckets[lock_id];
            size_t offset = bumped * BIT_SIZEOF(uint16_t);
            uint64_t dec = BIT(offset);

            atomic_fetch_sub(&bkt->raw_tally, dec);

            /* Set it to NONE here, inside the pi_lock, because
             * doing it outside would cause other threads to
             * acquire pi_lock and see BUMP, causing underflow */
            wtd->writer_state = WROOST_WRITER_NONE;
            wtd->want_lock_id = 0;

            spin_unlock(&curr->pi_lock, irql);
        } else {
            /* Does not need pi_lock because it was never BUMP to begin
             * with, because we never entered WAIT state, and never will
             * at this point, so no one acquiring our
             * lock will see stale state */
            wtd->writer_state = WROOST_WRITER_NONE;
            wtd->want_lock_id = 0;
        }
    }
}

static void post_read_release(struct rwlock *lock) {
    struct thread *curr = thread_get_current();
    struct wroost_thread_data *wtd = &curr->wroost_data;

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    wroost_check_enforce(wtd);
    enforce(wtd->read_nesting_depth > 0);

    wroost_id_t id = wroost_hash(lock);
    if (cc_likely(--wtd->read_nesting_depth == 0)) {
        for (int i = 0; i < WROOST_THREAD_COUNTER_BYTES; i++)
            wtd->counters[i] = 0;

#ifdef DEBUG_WROOST
        /* fine_valid = 0 automatically permits allocations,
         * we do not need to clear lock_id */
        bool found = false;
        for (int i = 0; i < WROOST_FINE_ENTRY_COUNT; i++) {
            if (wtd->fine_entries[i].lock_id == id) {
                found = true;
            } else {
                enforce(wtd->fine_entries[i].lock_id == 0);
                enforce((wtd->fine_valid & BIT(i)) == 0);
            }
        }

        enforce(found);
#endif

        wtd->fine_valid = 0;
        wtd->max_boost = 0;
        unboost_thread(curr);
    } else {
        uint16_t c_idx = id / WROOST_COARSE_FACTOR;

        wroost_counter_t prev_val = wroost_read_counter(wtd, c_idx);
        wroost_dec_counter(wtd, c_idx);

        if (prev_val < WROOST_COUNTER_MAX) {
            /* We are guaranteed to have no duplicates */
            for (int i = 0; i < WROOST_FINE_ENTRY_COUNT; i++) {
                struct wroost_fine_entry *wfe = &wtd->fine_entries[i];
                if ((wtd->fine_valid & BIT(i)) && wfe->lock_id == id) {
                    wroost_clear_fine_entry(wtd, i);
                    break;
                }
            }
        }
    }

    wroost_check_enforce(wtd);
    irql_lower(irql);
}

void wroost_post_release(struct rwlock *lock, enum rwlock_acquire_type type) {
    if (type == RWLOCK_READ)
        post_read_release(lock);

    /* Writers have nothing to do. They only use wroost to send
     * boosts to readers while they wait for readers, and once
     * the post_acquire happens, they are all done */
}

static bool percpu_boost_kick(cpu_id_t target, wroost_id_t lock_id,
                              int options) {
    struct wroost_percpu_data *pcd = global.schedulers[target]->wroost_data;

    size_t word = lock_id / BIT_SIZEOF(bitmap_word_t);
    size_t idx = lock_id % BIT_SIZEOF(bitmap_word_t);

    uint64_t prev_bits = 0;

    if (options & WROOST_KICK_UPDATE_PENDING)
        prev_bits = atomic_fetch_or(&pcd->pending_bitmap[word], BIT(idx));

    /* Early returning here is OK. We won't have a missed bit set, because
     * if this bit is already set, someone else has sent out the DPC and
     * it is waiting to get to this point */
    if (prev_bits & BIT(idx))
        return true;

    if (options & WROOST_KICK_SEND_DPCS) {
        enum wroost_state prev_state = atomic_fetch_or(
            &pcd->state, WROOST_DPC_PENDING | WROOST_DPC_ACTIVE);

        if (!(prev_state & WROOST_DPC_ACTIVE))
            return true;

        return dpc_enqueue_on_cpu(
            target, &global.schedulers[target]->wroost_data->boost_dpc);
    } else {
        return true;
    }
}

static size_t wroost_kick_cpus(struct cpu_mask *mask, wroost_id_t lock_id,
                               int options) {
    cpu_id_t i;
#ifdef DEBUG_WROOST
    cpu_mask_for_each(i, *mask)
        enforce(i < global.core_count);
#endif

    size_t kicked = 0;
    cpu_mask_for_each(i, *mask) {
        percpu_boost_kick(i, lock_id, options);
        kicked++;
    }

    return kicked;
}

size_t wroost_kick(struct wroost_kicks *kicks) {
    return wroost_kick_cpus(&kicks->cpus, kicks->lock_id,
                            WROOST_KICK_UPDATE_PENDING | WROOST_KICK_SEND_DPCS);
}

static void set_target_cpus(struct wroost_bucket *bkt, struct cpu_mask *on_mask,
                            enum thread_prio_class writer_prio) {
    cpu_id_t checked = 0;
    for (size_t bm_word_idx = 0; checked < global.core_count; bm_word_idx++) {
        bitmap_word_t word = atomic_load(&bkt->rq_min_prio[bm_word_idx]);

        for (int shift = 0;
             shift < BITMAP_BITS_PER_WORD && checked < global.core_count;
             shift += THREAD_PRIO_CLASS_BIT_COUNT) {
            enum thread_prio_class prio =
                (word >> shift) & THREAD_PRIO_CLASS_MASK;

            if (prio < writer_prio)
                cpu_mask_set(on_mask, checked);

            checked++;
        }
    }
}

/* Register/unregister a reader with a given CPU. Returns an actual
 * priority to boost to when add == true, base_prio_class otherwise */
static enum thread_prio_class update_reader(struct wroost_percpu_data *pcd,
                                            struct thread *t, bool add) {
    struct wroost_thread_data *wtd = &t->wroost_data;
    enum thread_prio_class boost_to = t->base_prio_class;
    enum thread_prio_class p;
    /* wtd->read_nesting_depth <= 4 not necessarily mean that all locks
     * held reside in the fine entries, such as when
     * a counter overflows at read_nesting_depth == 3 */
    size_t found_fine_count = 0;
    wroost_id_t found_coarse = 0;
    bool blocked_on_ts = atomic_load_acq(&t->blocked_ts);

    if (blocked_on_ts && add)
        thread_pin(t);

    /* Because coarse entries are either fully inside the fine entries or not,
     * we can use this without needing to check the actual coarse entries */
    BITMAP_DECLARE(skip_map, WROOST_THREAD_COUNTERS) = {0};

    for (int i = 0; i < WROOST_FINE_ENTRY_COUNT; i++) {
        if (wtd->fine_valid & BIT(i)) {
            wroost_id_t id = wtd->fine_entries[i].lock_id;
            if (add && (p = ceil_for(id, wtd->max_boost, boost_to)) > boost_to)
                boost_to = p;

            if (add) {
                percpu_inc_id(pcd, id, t->base_prio_class);
            } else {
                percpu_dec_id(pcd, id);
            }

            found_coarse = id / WROOST_COARSE_FACTOR;

            bitmap_set(skip_map, found_coarse);
            if (++found_fine_count == wtd->read_nesting_depth)
                break;
        }
    }

    enforce(found_fine_count <= wtd->read_nesting_depth);
    if (add) {
        if (blocked_on_ts) {
            hlist_add_head(&wtd->map_node, &pcd->blocked_ts_readers);
            wtd->in_blocked_ts_list = true;
        } else {
            if (wtd->read_nesting_depth == 1) {
                hlist_add_head(&wtd->map_node, &pcd->thread_map[found_coarse]);
            } else {
                hlist_add_head(&wtd->map_node, &pcd->multi_lock_readers);
            }
        }
    } else {
        hlist_del(&wtd->map_node);
    }

    /* No need to check coarse entries */
    if (found_fine_count == wtd->read_nesting_depth)
        goto out;

    wroost_counter_t val;
    wroost_id_t i;
    wroost_for_each_set_ctr(val, i, wtd->counters,
                            WROOST_THREAD_COUNTER_BYTES) {
        if (bitmap_test(skip_map, i))
            continue;

        for (size_t j = 0; j < WROOST_COARSE_FACTOR; j++) {
            wroost_id_t k = (wroost_id_t) i + j;

            if (add) {
                percpu_inc_id(pcd, k, t->base_prio_class);
            } else {
                percpu_dec_id(pcd, k);
            }

            if (add && (p = ceil_for(k, wtd->max_boost, boost_to)) > boost_to)
                boost_to = p;
        }
    }

out:
    if (!add && wtd->in_blocked_ts_list) {
        thread_unpin(t);
        wtd->in_blocked_ts_list = false;
    }

    return boost_to;
}

static void switch_out_writer(struct thread *t) {
    struct wroost_percpu_data *pcd = smp_core_scheduler()->wroost_data;

    spin_guard_high(&t->pi_lock);

    enum thread_prio_class writer_class = t->perceived_prio_class;
    wroost_id_t lock_id = t->wroost_data.want_lock_id;
    struct wroost_bucket *bkt = &wroost_global.hash_table.buckets[lock_id];
    size_t offset = writer_class * BIT_SIZEOF(uint16_t);
    uint64_t inc = BIT(offset);
    uint64_t prev_raw = atomic_fetch_add(&bkt->raw_tally, inc);

    t->wroost_data.writer_state = WROOST_WRITER_BUMP;

    /* 0 -> 1 transition, we handle IPI'ing and whatnot */
    uint16_t prev_class_cnt = (prev_raw >> offset) & UINT16_MAX;

    if (prev_class_cnt == 0) {
        set_target_cpus(bkt, &pcd->dpc_enqueue_deferred, writer_class);
        pcd->writer_lock_id = lock_id;
    }
}

static void switch_out_reader(struct thread *t) {
    struct wroost_thread_data *wtd = &t->wroost_data;

    if (wtd->read_nesting_depth == 0)
        return;

    wroost_check_enforce(wtd);
    struct wroost_percpu_data *pcd = smp_core_scheduler()->wroost_data;

    enum thread_prio_class boost_to = update_reader(pcd, t, /* add = */ true);

    boost_thread_to(t, boost_to);
}

/* Runs in scheduler with runqueue lock held */
void wroost_switch_out(struct thread *t) {
    if (!t)
        return;

    /* We only switch out the writer on WRITER_WAIT,
     * because it could've just been preempted. We do want
     * to check the reader data before the writer, since
     * writers go off of perceived priority, which reading influences */
    switch_out_reader(t);
    if (t->wroost_data.writer_state == WROOST_WRITER_WAIT)
        switch_out_writer(t);
}

/* Runs in scheduler with runqueue lock held, not necessarily on `t` */
void wroost_switch_in(struct thread *t) {
    if (t && t->wroost_data.read_nesting_depth > 0) {
        struct wroost_percpu_data *pcd = smp_core_scheduler()->wroost_data;
        update_reader(pcd, t, /* add = */ false);
    }
}

/* Separate from switch_in. this runs deferred DPC
 * notifications/IPIs onto the other CPUs */
void wroost_scheduler_switch_in_hook(void) {
    struct wroost_percpu_data *pcd = smp_core_scheduler()->wroost_data;
    if (wroost_kick_cpus(&pcd->dpc_enqueue_deferred, pcd->writer_lock_id,
                         WROOST_KICK_UPDATE_PENDING | WROOST_KICK_SEND_DPCS)) {
        cpu_mask_clear_all(&pcd->dpc_enqueue_deferred);
    } else {
#ifdef DEBUG_WROOST
        /* No bits can be set. writer_lock_id can be any value though, since
         * it just means that we didn't happen to
         * encounter any CPUs that need a kick */
        enforce(cpu_mask_first_set(&pcd->dpc_enqueue_deferred) ==
                CPU_MASK_BITS);
#endif
    }

    pcd->writer_lock_id = 0;
}

void wroost_pre_wait(struct thread *curr) {
#ifdef DEBUG_WROOST
    enforce(curr == thread_get_current());
#endif

    enforce(curr->wroost_data.writer_state == WROOST_WRITER_NONE ||
            curr->wroost_data.writer_state == WROOST_WRITER_WANT);
    if (curr->wroost_data.writer_state == WROOST_WRITER_WANT)
        curr->wroost_data.writer_state = WROOST_WRITER_WAIT;
}

/* This exists for the specific purpose of updating the wait
 * state for threads that began a wait but never actually got
 * switched out. We go back to wanting a lock if that happened */
void wroost_post_wait(struct thread *curr) {
#ifdef DEBUG_WROOST
    enforce(curr == thread_get_current());
#endif

    /* We either have to have (1) bumped the tallies, due to switch-out,
     * (2) begun a wait, but get bothered before the tally bump, or (3)
     * never been a writer at all. There is no case where this code
     * would be reached in the middle of trying to get the lock though. */
    enforce(curr->wroost_data.writer_state != WROOST_WRITER_WANT);
    if (curr->wroost_data.writer_state == WROOST_WRITER_WAIT)
        curr->wroost_data.writer_state = WROOST_WRITER_WANT;
}

void wroost_post_migrate_hook(struct thread *t, cpu_id_t from, cpu_id_t to) {
    /* The migration hook is only relevant for readers.
     * Writers keep no per-CPU state */
    if (t->wroost_data.read_nesting_depth == 0)
        return;

    if (atomic_load(&t->blocked_ts))
        panic("reader with blocked_ts cannot migrate");

    update_reader(global.schedulers[from]->wroost_data, t,
                  /* add = */ false);
    update_reader(global.schedulers[to]->wroost_data, t, /* add = */ true);
}

struct wroost_kicks wroost_post_prio_change_locked(struct thread *t,
                                                   enum thread_prio_class old,
                                                   enum thread_prio_class new) {
    struct wroost_kicks kicks = {0};
    SPINLOCK_ASSERT_HELD(&t->pi_lock);
    wroost_id_t lock_id = 0;
    if (t->wroost_data.writer_state == WROOST_WRITER_BUMP && old != new) {
        lock_id = t->wroost_data.want_lock_id;
        struct wroost_bucket *bkt = &wroost_global.hash_table.buckets[lock_id];
        wroost_move_tally(bkt, old, new);

        /* We need to go bother other CPUs. The problem is that the caller
         * can hold any lock (including the runqueue lock), which can
         * deadlock the target CPUs if they're trying to get the locks
         * the caller happens to be holding */
        if (old < new) {
            kicks.need_kicks = true;
            set_target_cpus(bkt, &kicks.cpus, new);
        }
    }

    return kicks;
}

static size_t wroost_calc_default_buckets(size_t cpu_count) {
    size_t target = cpu_count * WROOST_BUCKETS_PER_CPU_MULT;

    if (target <= WROOST_MIN_BUCKET_COUNT)
        return WROOST_MIN_BUCKET_COUNT;

    if (target >= WROOST_MAX_BUCKET_COUNT)
        return WROOST_MAX_BUCKET_COUNT;

    return CLAMP(next_pow2(target), WROOST_MIN_BUCKET_COUNT,
                 WROOST_MAX_BUCKET_COUNT);
}

void wroost_init(void) {
    size_t bucket_count = wroost_calc_default_buckets(global.core_count);
    enforce(IS_POW2(bucket_count));
    enforce(bucket_count <= UINT16_MAX - 1);

    wroost_global.hash_table.bucket_count = bucket_count;

    enforce(IS_POW2(wroost_global.hash_table.bucket_count));

    wroost_global.hash_table.buckets = nofail_vnews(
        wroost_global.hash_table.buckets, bucket_count, ALLOC_ZERO);

    for (size_t i = 0; i < bucket_count; i++) {
        struct wroost_bucket *bkt = &wroost_global.hash_table.buckets[i];

        /* Fill it with ones 'URGENT' */
        bitmap_fill((bitmap_word_t *) bkt->rq_min_prio,
                    THREAD_PRIO_CLASS_BIT_COUNT * CPU_MASK_BITS);
    }

    wroost_global.fine_index_bits = BITS_NEEDED(
        (wroost_global.hash_table.bucket_count / WROOST_THREAD_COUNTERS) - 1);

    wroost_global.fine_offset_mask =
        (1ULL << wroost_global.fine_index_bits) - 1;
}

void wroost_scheduler_init(struct scheduler *sched) {
    sched->wroost_data = nofail_vnew(sched->wroost_data, ALLOC_ZERO);

    struct wroost_percpu_data *pcd = sched->wroost_data;
    dpc_init(&pcd->boost_dpc, wroost_dpc, NULL, NULL);
    pcd->id = sched->core_id;

    pcd->pending_bitmap = nofail_vnews(
        pcd->pending_bitmap, BITMAP_WORDS(bucket_count()), ALLOC_ZERO);

    pcd->process_result.seen_data =
        nofail_vnews(pcd->process_result.seen_data,
                     BITMAP_WORDS(bucket_count()), ALLOC_ZERO);

    pcd->in_dpc_deferred_ids = nofail_vnews(
        pcd->in_dpc_deferred_ids, BITMAP_WORDS(bucket_count()), ALLOC_ZERO);

    pcd->rq_bucket_counts =
        nofail_vnews(pcd->rq_bucket_counts, bucket_count(), ALLOC_ZERO);

    cpu_mask_clear_all(&pcd->dpc_enqueue_deferred);
}
