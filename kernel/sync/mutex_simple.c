#include <sch/sched.h>
#include <sync/mutex_simple.h>
#include <sync/spinlock.h>
#include <thread/thread.h>

#ifdef DEBUG_LOCK_CHK

#include "lock_chk_internal.h"

struct mutex_simple_chk_acquire_state {
    struct lock_chk_acquire_request request;
    struct lock_chk_acquire_token token;
};

struct mutex_simple_chk_release_state {
    struct lock_chk_release_request request;
    struct lock_chk_release_token token;
};

static void
mutex_simple_chk_before_lock(struct mutex_simple_chk_acquire_state *state,
                             struct mutex_simple *m, unsigned int subclass,
                             const struct lock_chk_site *site) {
    lock_chk_note_lock_use(m->chk_initialized, m->chk_flags, &m->chk_used,
                           false, false);
    state->request = lock_chk_acquire_request_make(
        &m->chk_map, m, site, m->chk_flags, LOCK_CHK_TYPE_MUTEX_SIMPLE,
        LOCK_CHK_MODE_EXCLUSIVE, LOCK_CHK_WAIT_BLOCKING, subclass, false,
        false);
    lock_chk_before_acquire(&state->token, &state->request);
}

static void
mutex_simple_chk_locked(struct mutex_simple_chk_acquire_state *state) {
    lock_chk_acquired(&state->token);
}

static void
mutex_simple_chk_before_unlock(struct mutex_simple_chk_release_state *state,
                               struct mutex_simple *m,
                               const struct lock_chk_site *site) {
    state->request = lock_chk_release_request_make(
        &m->chk_map, m, site, m->chk_flags, LOCK_CHK_TYPE_MUTEX_SIMPLE,
        LOCK_CHK_MODE_EXCLUSIVE);
    lock_chk_before_release(&state->token, &state->request);
}

static void
mutex_simple_chk_unlocked(struct mutex_simple_chk_release_state *state) {
    lock_chk_released(&state->token);
}

static void mutex_simple_chk_state_init(struct mutex_simple *m,
                                        const struct lock_chk_class *class,
                                        enum lock_chk_flags flags) {
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    kassert(flags == LOCK_UNCHKD || class != NULL);
    m->chk_flags = flags;
    m->chk_initialized = true;
    atomic_store_explicit(&m->chk_used, false, memory_order_relaxed);
    lock_chk_map_runtime_init(&m->chk_map, class);
}

void mutex_simple_set_chk_flags(struct mutex_simple *m,
                                enum lock_chk_flags flags) {
    kassert(m->chk_initialized);
    kassert(m->owner == NULL);
    kassert(list_empty(&m->waiters.list));
    kassert(!spinlock_held(&m->waiters.lock));
    kassert(!spinlock_held(&m->lock));
    kassert(!atomic_load_explicit(&m->chk_used, memory_order_relaxed));
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    m->chk_flags = flags;
}

void mutex_simple_reinit_chk(struct mutex_simple *m,
                             const struct lock_chk_class *class,
                             enum lock_chk_flags flags) {
    kassert(m->chk_initialized);
    kassert(m->owner == NULL);
    kassert(list_empty(&m->waiters.list));
    kassert(!spinlock_held(&m->waiters.lock));
    kassert(!spinlock_held(&m->lock));
    mutex_simple_init_chk_internal(m, class, flags);
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
                             struct mutex_simple *m, unsigned int subclass,
                             const struct lock_chk_site *site) {
    unused(state, m, subclass, site);
}

static inline void
mutex_simple_chk_locked(struct mutex_simple_chk_acquire_state *state) {
    unused(state);
}

static inline void
mutex_simple_chk_before_unlock(struct mutex_simple_chk_release_state *state,
                               struct mutex_simple *m,
                               const struct lock_chk_site *site) {
    unused(state, m, site);
}

static inline void
mutex_simple_chk_unlocked(struct mutex_simple_chk_release_state *state) {
    unused(state);
}

static void mutex_simple_chk_state_init(struct mutex_simple *m,
                                        const struct lock_chk_class *class,
                                        enum lock_chk_flags flags) {
    unused(m, class, flags);
}

void mutex_simple_set_chk_flags(struct mutex_simple *m,
                                enum lock_chk_flags flags) {
    unused(m, flags);
}

void mutex_simple_reinit_chk(struct mutex_simple *m,
                             const struct lock_chk_class *class,
                             enum lock_chk_flags flags) {
    mutex_simple_init_chk_internal(m, class, flags);
}

#endif /* DEBUG_LOCK_CHK */

static void mutex_simple_sanity_check(void) {
    kassert(irq_in_thread_context());
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
    bool active = owner && atomic_load(&owner->state) == THREAD_STATE_RUNNING;
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
    thread_block_on(&m->waiters, THREAD_WAIT_UNINTERRUPTIBLE, m);
    spin_unlock(&m->lock, irql);
    scheduler_yield();
}

void mutex_simple_init_chk_internal(struct mutex_simple *m,
                                    const struct lock_chk_class *class,
                                    enum lock_chk_flags flags) {
    m->owner = NULL;
    INIT_LIST_HEAD(&m->waiters.list);
    spinlock_init(&m->waiters.lock, LOCK_UNCHKD);
    spinlock_init(&m->lock, LOCK_UNCHKD);
    mutex_simple_chk_state_init(m, class, flags);
}

void mutex_simple_lock_subclass_internal(struct mutex_simple *m,
                                         unsigned int subclass,
                                         const struct lock_chk_site *site) {
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

void mutex_simple_lock_internal(struct mutex_simple *m,
                                const struct lock_chk_site *site) {
    mutex_simple_lock_subclass_internal(m, 0, site);
}

void mutex_simple_unlock_internal(struct mutex_simple *m,
                                  const struct lock_chk_site *site) {
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

    struct thread *next = thread_queue_pop_front(&m->waiters);
    if (next != NULL)
        thread_wake(next, THREAD_WAKE_REASON_BLOCKING_MANUAL,
                    next->perceived_prio_class, m);

    spin_unlock(&m->lock, irql);

    mutex_simple_chk_unlocked(&chk_state);
}
