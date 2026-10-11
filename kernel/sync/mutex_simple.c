#include <sch/sched.h>
#include <sync/mutex_simple.h>
#include <sync/spinlock.h>
#include <thread/thread.h>

#ifdef DEBUG_LOCK_CHK

#include "lock_chk/internal.h"

static inline void mutex_simple_chk_stamp(struct mutex_simple *lock) {
    lock->chk.instance = lock;
    lock->chk.type = LOCK_CHK_TYPE_MUTEX_SIMPLE;
}

struct mutex_simple_chk_acquire_state {
    struct lock_chk_acq_req request;
    struct lock_chk_acq_token token;
};

struct mutex_simple_chk_release_state {
    struct lock_chk_rel_req request;
    struct lock_chk_rel_token token;
};

static void
mutex_simple_chk_before_lock(struct mutex_simple_chk_acquire_state *state,
                             struct mutex_simple *m, uint8_t subclass,
                             const struct lock_chk_site *site) {
    enum lock_op_flags flags = LOCK_OP_IRQ_NONE | LOCK_OP_KIND_BLOCKING;
    lock_chk_note_use(&m->chk, flags);
    mutex_simple_chk_stamp(m);
    state->request = lock_chk_acq_req_make(
        &m->chk, site, LOCK_CHK_MODE_EXCLUSIVE, subclass, flags);
    lock_chk_before_acq(&state->token, &state->request);
}

static void
mutex_simple_chk_locked(struct mutex_simple_chk_acquire_state *state) {
    lock_chk_acqd(&state->token);
}

static void
mutex_simple_chk_before_unlock(struct mutex_simple_chk_release_state *state,
                               struct mutex_simple *m,
                               const struct lock_chk_site *site) {
    mutex_simple_chk_stamp(m);
    state->request =
        lock_chk_rel_req_make(&m->chk, site, LOCK_CHK_MODE_EXCLUSIVE);
    lock_chk_before_rel(&state->token, &state->request);
}

static void
mutex_simple_chk_unlocked(struct mutex_simple_chk_release_state *state) {
    lock_chk_reld(&state->token);
}

static void mutex_simple_chk_state_init(struct mutex_simple *m,
                                        const struct lock_chk_class *class,
                                        enum lock_chk_flags flags) {
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    kassert(flags == LOCK_UNCHKD || class != NULL);
    m->chk.flags = flags;
    m->chk.initialized = true;
    atomic_store_rlx(&m->chk.used, false);
    lock_chk_map_runtime_init(&m->chk.map, class);
}

void mutex_simple_set_chk_flags(struct mutex_simple *m,
                                enum lock_chk_flags flags) {
    kassert(m->chk.initialized);
    kassert(m->owner == NULL);
    kassert(list_empty(&m->waiters.waiters));
    kassert(!spinlock_locked(&m->waiters.lock));
    kassert(!spinlock_locked(&m->lock));
    kassert(!atomic_load_rlx(&m->chk.used));
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    m->chk.flags = flags;
}

void mutex_simple_reinit_chk(struct mutex_simple *m,
                             const struct lock_chk_class *class,
                             enum lock_chk_flags flags) {
    kassert(m->chk.initialized);
    kassert(m->owner == NULL);
    kassert(list_empty(&m->waiters.waiters));
    kassert(!spinlock_locked(&m->waiters.lock));
    kassert(!spinlock_locked(&m->lock));
    mutex_simple_init_chk_full(m, class, flags);
}

#else /* !defined(DEBUG_LOCK_CHK) */

struct mutex_simple_chk_acquire_state {
    bool unused;
};

struct mutex_simple_chk_release_state {
    bool unused;
};

static inline void
mutex_simple_chk_before_lock(struct mutex_simple_chk_acquire_state *state,
                             struct mutex_simple *m, uint8_t subclass,
                             const struct lock_chk_site *site) {
    cc_unused(state, m, subclass, site);
}

static inline void
mutex_simple_chk_locked(struct mutex_simple_chk_acquire_state *state) {
    cc_unused(state);
}

static inline void
mutex_simple_chk_before_unlock(struct mutex_simple_chk_release_state *state,
                               struct mutex_simple *m,
                               const struct lock_chk_site *site) {
    cc_unused(state, m, site);
}

static inline void
mutex_simple_chk_unlocked(struct mutex_simple_chk_release_state *state) {
    cc_unused(state);
}

static void mutex_simple_chk_state_init(struct mutex_simple *m,
                                        const struct lock_chk_class *class,
                                        enum lock_chk_flags flags) {
    cc_unused(m, class, flags);
}

void mutex_simple_set_chk_flags(struct mutex_simple *m,
                                enum lock_chk_flags flags) {
    cc_unused(m, flags);
}

void mutex_simple_reinit_chk(struct mutex_simple *m,
                             const struct lock_chk_class *class,
                             enum lock_chk_flags flags) {
    mutex_simple_init_chk_full(m, class, flags);
}

#endif /* DEBUG_LOCK_CHK */

static void mutex_simple_sanity_check(void) {
    kassert(irq_not_in_interrupt());
    kassert(irql_get() <= IRQL_APC_LEVEL);
}

static bool try_acquire_simple_mutex(struct mutex_simple *m,
                                     struct thread *curr) {
    enum irql irql = spin_lock(&m->lock);
    if (m->owner == NULL) {
        m->owner = curr;
        spin_unlock(&m->lock, irql);
        return true;
    }
    spin_unlock(&m->lock, irql);
    return false;
}

static bool should_spin_on_mutex(struct mutex_simple *m) {
    enum irql irql = spin_lock(&m->lock);
    struct thread *owner = m->owner;
    bool active =
        owner && atomic_load_rlx(&owner->state) == THREAD_STATE_RUNNING;
    spin_unlock(&m->lock, irql);
    return active;
}

static bool spin_wait_simple_mutex(struct mutex_simple *m,
                                   struct thread *curr) {
    for (int i = 0; i < 500; i++)
        if (try_acquire_simple_mutex(m, curr))
            return true;

    return false;
}

static void block_on_simple_mutex(struct mutex_simple *m) {
    enum irql irql = spin_lock(&m->lock);
    /* owner can release between failed fast path and this lock */
    if (!m->owner) {
        spin_unlock(&m->lock, irql);
        return;
    }
    thread_wait_prepare_one(&m->waiters, m, THREAD_WAIT_UNINTERRUPTIBLE,
                            THREAD_BLOCK_REASON_MANUAL);
    spin_unlock(&m->lock, irql);
    thread_wait_complete();
}

void mutex_simple_init_chk_full(struct mutex_simple *m,
                                const struct lock_chk_class *class,
                                enum lock_chk_flags flags) {
    m->owner = NULL;
    INIT_LIST_HEAD(&m->waiters.waiters);
    spinlock_init(&m->waiters.lock, LOCK_UNCHKD);
    spinlock_init(&m->lock, LOCK_UNCHKD);
    mutex_simple_chk_state_init(m, class, flags);
}

void mutex_simple_lock_subclass_full(struct mutex_simple *m, uint8_t subclass,
                                     const struct lock_chk_site *site)
    TSA_NO_ANALYSIS {
    mutex_simple_sanity_check();

    struct mutex_simple_chk_acquire_state chk_state;
    mutex_simple_chk_before_lock(&chk_state, m, subclass, site);

    struct thread *curr = thread_get_current();

    while (true) {
        if (try_acquire_simple_mutex(m, curr))
            break;

        if (should_spin_on_mutex(m))
            if (spin_wait_simple_mutex(m, curr))
                break;

        block_on_simple_mutex(m);
    }

    kassert(m->owner == curr);
    mutex_simple_chk_locked(&chk_state);
}

void mutex_simple_lock_full(struct mutex_simple *m,
                            const struct lock_chk_site *site) TSA_NO_ANALYSIS {
    mutex_simple_lock_subclass_full(m, 0, site);
}

void mutex_simple_unlock_full(
    struct mutex_simple *m, const struct lock_chk_site *site) TSA_NO_ANALYSIS {
    mutex_simple_sanity_check();

    struct thread *curr = thread_get_current();

    if (m->owner != curr) {
        panic("mutex_simple unlock by non-owner thread. owner is %p, current "
              "is %p",
              m->owner, curr);
    }

    struct mutex_simple_chk_release_state chk_state;
    mutex_simple_chk_before_unlock(&chk_state, m, site);

    enum irql irql = spin_lock(&m->lock);

    m->owner = NULL;

    thread_wait_header_satisfy(&m->waiters, THREAD_WAKE_REASON_BLOCKING_MANUAL,
                               NULL);

    spin_unlock(&m->lock, irql);

    mutex_simple_chk_unlocked(&chk_state);
}

bool mutex_simple_locked(struct mutex_simple *m) {
    return mutex_simple_get_owner(m) != NULL;
}

struct thread *mutex_simple_get_owner(struct mutex_simple *m) {
    enum irql irql = spin_lock(&m->lock);
    struct thread *owner = m->owner;
    spin_unlock(&m->lock, irql);
    return owner;
}

void mutex_simple_assert_held_full(struct mutex_simple *m,
                                   const struct lock_chk_site *site) {
#ifdef DEBUG_LOCK_CHK
    mutex_simple_chk_stamp(m);
    if (m->chk.flags != LOCK_UNCHKD && lock_chk_tracking_active() &&
        lock_chk_assert_held_deep(&m->chk, LOCK_CHK_MODE_IGNORED,
                                  /*want_held=*/true, site))
        return;
#else
    cc_unused(site);
#endif
    kassert(mutex_simple_get_owner(m) == thread_get_current(),
            "mutex_simple not held by current thread");
}

void mutex_simple_assert_not_held_full(struct mutex_simple *m,
                                       const struct lock_chk_site *site) {
#ifdef DEBUG_LOCK_CHK
    mutex_simple_chk_stamp(m);
    if (m->chk.flags != LOCK_UNCHKD && lock_chk_tracking_active() &&
        lock_chk_assert_held_deep(&m->chk, LOCK_CHK_MODE_IGNORED,
                                  /*want_held=*/false, site))
        return;
#else
    cc_unused(site);
#endif
    kassert(mutex_simple_get_owner(m) != thread_get_current(),
            "mutex_simple unexpectedly held by current thread");
}
