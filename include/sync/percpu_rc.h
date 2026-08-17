/* @title: Per-CPU Reference Counter */
#pragma once
#include <compiler.h>
#include <kassert.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/rcu.h>

/*
 * Per-CPU Reference Counter
 *
 * Fast, no contention reference counting by distributing RC inc/dec across
 * per-CPU counters in fast path, using large bias and RCU GP to transition
 * to atomic counter without lost updates */

struct percpu_rc;
typedef void (*percpu_rc_release_fn)(struct percpu_rc *);

enum percpu_rc_flags {
    PERCPU_RC_INIT_ATOMIC = 1 << 0,
    PERCPU_RC_ALLOW_REINIT = 1 << 1,
};

#define PERCPU_COUNT_BIAS (1LL << 30)

#define PERCPU_RC_DEAD (1UL << 0)
#define PERCPU_RC_ATOMIC (1UL << 1)
#define PERCPU_RC_PTR_MASK (~(PERCPU_RC_DEAD | PERCPU_RC_ATOMIC))

struct percpu_rc {
    _Atomic int64_t count;
    _Atomic uintptr_t percpu_count_ptr;
    percpu_rc_release_fn release;
    bool allow_reinit;
    struct rcu_cb rcu;
} __cache_aligned;

int percpu_rc_init(struct percpu_rc *ref, percpu_rc_release_fn release,
                   enum percpu_rc_flags flags);
void percpu_rc_destroy(struct percpu_rc *ref);
void percpu_rc_kill(struct percpu_rc *ref);
void percpu_rc_reinit(struct percpu_rc *ref);
void percpu_rc_resurrect(struct percpu_rc *ref);
int64_t percpu_rc_read(struct percpu_rc *ref);

static inline bool percpu_rc_is_percpu(uintptr_t pcpu) {
    return (pcpu & (PERCPU_RC_DEAD | PERCPU_RC_ATOMIC)) == 0;
}

static inline void percpu_rc_get(struct percpu_rc *ref) {
    rcu_read_lock();
    uintptr_t pcpu =
        atomic_load_explicit(&ref->percpu_count_ptr, memory_order_relaxed);

    if (likely(percpu_rc_is_percpu(pcpu))) {
        int64_t *counters = (int64_t *) (pcpu & PERCPU_RC_PTR_MASK);
        counters[smp_core_id()]++;
    } else {
        atomic_fetch_add_explicit(&ref->count, 1, memory_order_relaxed);
    }
    rcu_read_unlock();
}

static inline void percpu_rc_put(struct percpu_rc *ref) {
    rcu_read_lock();
    uintptr_t pcpu =
        atomic_load_explicit(&ref->percpu_count_ptr, memory_order_relaxed);

    if (likely(percpu_rc_is_percpu(pcpu))) {
        int64_t *counters = (int64_t *) (pcpu & PERCPU_RC_PTR_MASK);
        counters[smp_core_id()]--;
        rcu_read_unlock();
    } else {
        rcu_read_unlock();
        if (atomic_fetch_sub_explicit(&ref->count, 1, memory_order_acq_rel) ==
            1) {
            if (ref->release)
                ref->release(ref);
        }
    }
}

static inline bool percpu_rc_tryget(struct percpu_rc *ref) {
    rcu_read_lock();
    uintptr_t pcpu =
        atomic_load_explicit(&ref->percpu_count_ptr, memory_order_relaxed);

    if (likely(percpu_rc_is_percpu(pcpu))) {
        int64_t *counters = (int64_t *) (pcpu & PERCPU_RC_PTR_MASK);
        counters[smp_core_id()]++;
        rcu_read_unlock();
        return true;
    }

    rcu_read_unlock();
    int64_t c = atomic_load_explicit(&ref->count, memory_order_relaxed);
    do {
        if (c <= 0)
            return false;
    } while (!atomic_compare_exchange_weak_explicit(
        &ref->count, &c, c + 1, memory_order_relaxed, memory_order_relaxed));
    return true;
}

static inline bool percpu_rc_tryget_live(struct percpu_rc *ref) {
    rcu_read_lock();
    uintptr_t pcpu =
        atomic_load_explicit(&ref->percpu_count_ptr, memory_order_relaxed);

    if (likely(percpu_rc_is_percpu(pcpu))) {
        int64_t *counters = (int64_t *) (pcpu & PERCPU_RC_PTR_MASK);
        counters[smp_core_id()]++;
        rcu_read_unlock();
        return true;
    }

    if (pcpu & PERCPU_RC_DEAD) {
        rcu_read_unlock();
        return false;
    }

    rcu_read_unlock();
    int64_t c = atomic_load_explicit(&ref->count, memory_order_relaxed);
    do {
        if (c <= 0)
            return false;
    } while (!atomic_compare_exchange_weak_explicit(
        &ref->count, &c, c + 1, memory_order_relaxed, memory_order_relaxed));
    return true;
}

static inline bool percpu_rc_is_zero(struct percpu_rc *ref) {
    return atomic_load_explicit(&ref->count, memory_order_relaxed) == 0;
}

static inline bool percpu_rc_is_dying(struct percpu_rc *ref) {
    return (atomic_load_explicit(&ref->percpu_count_ptr, memory_order_relaxed) &
            PERCPU_RC_DEAD) != 0;
}
