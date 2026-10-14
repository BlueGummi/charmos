/* @title: Per-CPU Reference Counter */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <enforce.h>
#include <math/bit.h>
#include <smp/core.h>
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
    PERCPU_RC_INIT_ATOMIC = BIT(0),
    PERCPU_RC_ALLOW_REINIT = BIT(1),
};

#define PERCPU_COUNT_BIAS (1LL << 30)

#define PERCPU_RC_DEAD BIT(0)
#define PERCPU_RC_ATOMIC BIT(1)
#define PERCPU_RC_PTR_MASK (~(PERCPU_RC_DEAD | PERCPU_RC_ATOMIC))
#define PERCPU_RC_PTR(m) (atomic_int64_t *) (m & PERCPU_RC_PTR_MASK)

struct percpu_rc {
    atomic_int64_t count;
    atomic_uintptr_t percpu_count_ptr;
    percpu_rc_release_fn release;
    bool allow_reinit;
    struct rcu_cb rcu;
} cc_cache_aligned;

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

/* Notes on why TOPOC_NONE is fine here:
 *
 * percpu_rc is designed with the premise of "getting migrated, dec'ing the
 * wrong counter, and all that other business will cause no problem".
 *
 * the eventual kill will deal with all the +'s and -'s,
 * meaning that if you end up with
 *
 * CPU0: rc == 9
 * CPU1: rc == -9
 *
 * the eventual cleanup goes "9 - 9 = 0" and handles it perfectly ok
 *
 */
static inline void percpu_rc_get(struct percpu_rc *ref) {
    rcu_read_lock();
    uintptr_t pcpu = atomic_load_rlx(&ref->percpu_count_ptr);

    if (cc_likely(percpu_rc_is_percpu(pcpu))) {
        atomic_int64_t *counters = PERCPU_RC_PTR(pcpu);
        atomic_inc_rlx(&counters[smp_id(TOPC_NONE)]);
    } else {
        atomic_inc_rlx(&ref->count);
    }
    rcu_read_unlock();
}

static inline void percpu_rc_put(struct percpu_rc *ref) {
    rcu_read_lock();
    uintptr_t pcpu = atomic_load_rlx(&ref->percpu_count_ptr);

    if (cc_likely(percpu_rc_is_percpu(pcpu))) {
        atomic_int64_t *counters = PERCPU_RC_PTR(pcpu);
        atomic_dec_rlx(&counters[smp_id(TOPC_NONE)]);
        rcu_read_unlock();
    } else {
        rcu_read_unlock();
        if (atomic_dec_and_test(&ref->count)) {
            if (ref->release)
                ref->release(ref);
        }
    }
}

static inline bool percpu_rc_tryget(struct percpu_rc *ref) {
    rcu_read_lock();
    uintptr_t pcpu = atomic_load_rlx(&ref->percpu_count_ptr);

    if (cc_likely(percpu_rc_is_percpu(pcpu))) {
        atomic_int64_t *counters = PERCPU_RC_PTR(pcpu);
        atomic_inc_rlx(&counters[smp_id(TOPC_NONE)]);
        rcu_read_unlock();
        return true;
    }

    rcu_read_unlock();
    int64_t c = atomic_load_rlx(&ref->count);
    do {
        if (c <= 0)
            return false;
    } while (!atomic_cas_weak(&ref->count, &c, c + 1, mo_rlx, mo_rlx));
    return true;
}

static inline bool percpu_rc_tryget_live(struct percpu_rc *ref) {
    rcu_read_lock();
    uintptr_t pcpu = atomic_load_rlx(&ref->percpu_count_ptr);

    if (cc_likely(percpu_rc_is_percpu(pcpu))) {
        atomic_int64_t *counters = PERCPU_RC_PTR(pcpu);
        atomic_inc_rlx(&counters[smp_id(TOPC_NONE)]);
        rcu_read_unlock();
        return true;
    }

    if (pcpu & PERCPU_RC_DEAD) {
        rcu_read_unlock();
        return false;
    }

    rcu_read_unlock();
    int64_t c = atomic_load_rlx(&ref->count);
    do {
        if (c <= 0)
            return false;
    } while (!atomic_cas_weak(&ref->count, &c, c + 1, mo_rlx, mo_rlx));
    return true;
}

static inline bool percpu_rc_is_zero(struct percpu_rc *ref) {
    return atomic_load_rlx(&ref->count) == 0;
}

static inline bool percpu_rc_is_dying(struct percpu_rc *ref) {
    return (atomic_load_rlx(&ref->percpu_count_ptr) & PERCPU_RC_DEAD) != 0;
}
