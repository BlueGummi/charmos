#include <errno.h>
#include <global.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <sync/percpu_rc.h>
#include <sync/rcu.h>

static void percpu_rc_switch_to_atomic_rcu(struct rcu_cb *cb, void *arg) {
    (void) cb;
    struct percpu_rc *rc = arg;
    uintptr_t pcpu =
        atomic_load_explicit(&rc->percpu_count_ptr, memory_order_relaxed);

    int64_t *counters = (int64_t *) (pcpu & PERCPU_RC_PTR_MASK);
    int64_t sum = 0;

    if (counters) {
        for (size_t i = 0; i < global.core_count; i++) {
            sum += counters[i];
            counters[i] = 0;
        }

        if (!rc->allow_reinit) {
            kfree(counters);
            atomic_store_explicit(&rc->percpu_count_ptr,
                                  PERCPU_RC_DEAD | PERCPU_RC_ATOMIC,
                                  memory_order_release);
        }
    }

    /* Add pcpu sum and remove bias + initial kill ref, == 0, release */
    int64_t adjustment = sum - (PERCPU_COUNT_BIAS + 1);
    int64_t prev =
        atomic_fetch_add_explicit(&rc->count, adjustment, memory_order_acq_rel);

    if (prev + adjustment == 0) {
        if (rc->release)
            rc->release(rc);
    }
}

int percpu_rc_init(struct percpu_rc *rc, percpu_rc_release_fn release,
                   enum percpu_rc_flags flags) {
    rc->release = release;
    rc->allow_reinit = (flags & PERCPU_RC_ALLOW_REINIT) != 0;

    if (flags & PERCPU_RC_INIT_ATOMIC) {
        atomic_store_explicit(&rc->count, 1, memory_order_relaxed);
        atomic_store_explicit(&rc->percpu_count_ptr, PERCPU_RC_ATOMIC,
                              memory_order_relaxed);
        return 0;
    }

    size_t size = sizeof(int64_t) * global.core_count;
    int64_t *counters = kmalloc(size, ALLOC_FLAGS_ZERO);
    if (!counters)
        return ERR_NO_MEM;

    /* Init with PERCPU_COUNT_BIAS + 1 so early put()s during kill don't
     * prematurely trigger release */
    atomic_store_explicit(&rc->count, 1 + PERCPU_COUNT_BIAS,
                          memory_order_relaxed);
    atomic_store_explicit(&rc->percpu_count_ptr, (uintptr_t) counters,
                          memory_order_release);

    return 0;
}

void percpu_rc_destroy(struct percpu_rc *ref) {
    uintptr_t pcpu =
        atomic_load_explicit(&ref->percpu_count_ptr, memory_order_relaxed);
    int64_t *counters = (int64_t *) (pcpu & PERCPU_RC_PTR_MASK);

    if (counters) {
        kfree(counters);
        atomic_store_explicit(&ref->percpu_count_ptr,
                              PERCPU_RC_DEAD | PERCPU_RC_ATOMIC,
                              memory_order_relaxed);
    }
}

void percpu_rc_kill(struct percpu_rc *ref) {
    uintptr_t pcpu =
        atomic_load_explicit(&ref->percpu_count_ptr, memory_order_relaxed);

    if (pcpu & PERCPU_RC_DEAD)
        return;

    atomic_fetch_or_explicit(&ref->percpu_count_ptr,
                             PERCPU_RC_DEAD | PERCPU_RC_ATOMIC,
                             memory_order_release);

    rcu_defer(&ref->rcu, percpu_rc_switch_to_atomic_rcu, ref);
}

void percpu_rc_reinit(struct percpu_rc *rc) {
    kassert(rc->allow_reinit,
            "percpu_rc was not initialized with allow_reinit");

    uintptr_t pcpu =
        atomic_load_explicit(&rc->percpu_count_ptr, memory_order_relaxed);
    int64_t *counters = (int64_t *) (pcpu & PERCPU_RC_PTR_MASK);
    kassert(counters, "percpu_rc counters missing during reinit");

    atomic_store_explicit(&rc->count, 1 + PERCPU_COUNT_BIAS,
                          memory_order_relaxed);
    atomic_store_explicit(&rc->percpu_count_ptr, (uintptr_t) counters,
                          memory_order_release);
}

void percpu_rc_resurrect(struct percpu_rc *rc) {
    atomic_store_explicit(&rc->count, 1, memory_order_relaxed);
}

int64_t percpu_rc_read(struct percpu_rc *rc) {
    uintptr_t pcpu =
        atomic_load_explicit(&rc->percpu_count_ptr, memory_order_relaxed);

    if (percpu_rc_is_percpu(pcpu)) {
        int64_t sum = 0;
        int64_t *counters = (int64_t *) (pcpu & PERCPU_RC_PTR_MASK);
        if (counters) {
            for (size_t i = 0; i < global.core_count; i++)
                sum += counters[i];
        }
        return (atomic_load_explicit(&rc->count, memory_order_relaxed) -
                PERCPU_COUNT_BIAS) +
               sum;
    }

    return atomic_load_explicit(&rc->count, memory_order_relaxed);
}
