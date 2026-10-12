#include <enforce.h>
#include <err.h>
#include <global.h>
#include <mem/alloc.h>
#include <sync/percpu_rc.h>
#include <sync/rcu.h>

static void percpu_rc_switch_to_atomic_rcu(struct rcu_cb *cb) {
    cc_unused(cb);
    struct percpu_rc *rc = container_of(cb, struct percpu_rc, rcu);
    uintptr_t pcpu = atomic_load_rlx(&rc->percpu_count_ptr);

    int64_t *counters = PERCPU_RC_PTR(pcpu);
    int64_t sum = 0;

    if (counters) {
        for (size_t i = 0; i < global.core_count; i++) {
            sum += counters[i];
            counters[i] = 0;
        }

        if (!rc->allow_reinit) {
            vfree(counters);
            atomic_store_release(&rc->percpu_count_ptr,
                                 PERCPU_RC_DEAD | PERCPU_RC_ATOMIC);
        }
    }

    /* Add pcpu sum and remove bias + initial kill ref, == 0, release */
    int64_t adjustment = sum - (PERCPU_COUNT_BIAS + 1);
    int64_t prev = atomic_fetch_add_acq_rel(&rc->count, adjustment);

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
        atomic_store_rlx(&rc->count, 1);
        atomic_store_rlx(&rc->percpu_count_ptr, PERCPU_RC_ATOMIC);
        return 0;
    }

    size_t size = sizeof(int64_t) * global.core_count;
    int64_t *counters = valloc(size, ALLOC_ZERO);
    if (!counters)
        return ERR_NO_MEM;

    /* Init with PERCPU_COUNT_BIAS + 1 so early put()s during kill don't
     * prematurely trigger release */
    atomic_store_rlx(&rc->count, 1 + PERCPU_COUNT_BIAS);
    atomic_store_release(&rc->percpu_count_ptr, (uintptr_t) counters);

    return 0;
}

void percpu_rc_destroy(struct percpu_rc *ref) {
    uintptr_t pcpu = atomic_load_rlx(&ref->percpu_count_ptr);
    int64_t *counters = PERCPU_RC_PTR(pcpu);

    if (counters) {
        vfree(counters);
        atomic_store_rlx(&ref->percpu_count_ptr,
                         PERCPU_RC_DEAD | PERCPU_RC_ATOMIC);
    }
}

void percpu_rc_kill(struct percpu_rc *ref) {
    uintptr_t pcpu = atomic_load_rlx(&ref->percpu_count_ptr);

    if (pcpu & PERCPU_RC_DEAD)
        return;

    atomic_fetch_or_release(&ref->percpu_count_ptr,
                            PERCPU_RC_DEAD | PERCPU_RC_ATOMIC);

    rcu_defer(&ref->rcu, percpu_rc_switch_to_atomic_rcu);
}

void percpu_rc_reinit(struct percpu_rc *rc) {
    enforce(rc->allow_reinit,
            "percpu_rc was not initialized with allow_reinit");

    uintptr_t pcpu = atomic_load_rlx(&rc->percpu_count_ptr);
    int64_t *counters = PERCPU_RC_PTR(pcpu);
    enforce(counters, "percpu_rc counters missing during reinit");

    atomic_store_rlx(&rc->count, 1 + PERCPU_COUNT_BIAS);
    atomic_store_release(&rc->percpu_count_ptr, (uintptr_t) counters);
}

void percpu_rc_resurrect(struct percpu_rc *rc) {
    atomic_store_rlx(&rc->count, 1);
}

int64_t percpu_rc_read(struct percpu_rc *rc) {
    uintptr_t pcpu = atomic_load_rlx(&rc->percpu_count_ptr);

    if (percpu_rc_is_percpu(pcpu)) {
        int64_t sum = 0;
        int64_t *counters = PERCPU_RC_PTR(pcpu);
        if (counters) {
            for (size_t i = 0; i < global.core_count; i++)
                sum += counters[i];
        }
        return (atomic_load_rlx(&rc->count) - PERCPU_COUNT_BIAS) + sum;
    }

    return atomic_load_rlx(&rc->count);
}
