#include <kassert.h>
#include <sch/apc.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <smp/core.h>

#define apc_from_list_node(n) container_of(n, struct apc, node)

static inline bool thread_has_apcs(struct thread *t) {
    return t->apc_pending_mask != 0;
}

static inline size_t apc_type_bit(enum apc_type t) {
    return (size_t) 1ULL << (size_t) t;
}

static inline bool apc_list_empty(struct thread *t, enum apc_type type) {
    return list_empty(&t->apc_head[type]);
}

static inline void apc_list_del(struct apc *a) {
    list_del(&a->node);
}

static inline void apc_add_tail(struct thread *t, struct apc *a,
                                enum apc_type type) {
    list_add_tail(&a->node, &t->apc_head[type]);
}

static inline void apc_list_unset_bitmask(struct thread *t,
                                          enum apc_type type) {
    atomic_fetch_and(&t->apc_pending_mask, ~apc_type_bit(type));
}

static inline void apc_set_cancelled(struct apc *a) {
    atomic_store(&a->cancelled, true);
}

static inline void apc_unset_cancelled(struct apc *a) {
    atomic_store(&a->cancelled, false);
}

static inline bool apc_is_cancelled(struct apc *a) {
    return atomic_load(&a->cancelled);
}

static inline bool thread_can_exec_special_apcs(struct thread *t) {
    return t->special_apc_disable == 0 &&
           (atomic_load(&t->apc_pending_mask) &
            apc_type_bit(APC_TYPE_SPECIAL_KERNEL));
}

static inline bool thread_can_exec_kernel_apcs(struct thread *t) {
    return t->kernel_apc_disable == 0 &&
           (atomic_load(&t->apc_pending_mask) & apc_type_bit(APC_TYPE_KERNEL));
}

static inline bool thread_is_dying(struct thread *t) {
    enum thread_state s = thread_get_state(t);
    return s == THREAD_STATE_TERMINATED || s == THREAD_STATE_ZOMBIE;
}

static bool thread_apc_sanity_check(struct thread *t) {
    if (unlikely(thread_get_state(t) == THREAD_STATE_IDLE_THREAD))
        k_panic("Attempted to put an APC on the idle thread");

    if (unlikely(thread_is_dying(t)))
        return false;

    return true;
}

static void apc_execute(struct apc *a) {
    enum irql old = irql_raise(IRQL_APC_LEVEL);

    struct thread *curr = scheduler_get_curr_thread();

    curr->executing_apc = true;

    a->func(a, a->arg1, a->arg2);

    curr->executing_apc = false;
    curr->total_apcs_ran++;

    irql_lower(old);
}

static void deliver_apc_type(struct thread *t, enum apc_type type) {
    struct apc *apc;

    while (true) {
        enum irql irql = thread_acquire(t);

        if (apc_list_empty(t, type)) {
            apc_list_unset_bitmask(t, type);
            thread_release(t, irql);
            return;
        }

        apc = list_first_entry(&t->apc_head[type], struct apc, node);
        apc_list_del(apc);
        apc->enqueued = false;

        thread_release(t, irql);

        if (!apc_is_cancelled(apc))
            apc_execute(apc);
    }
}

static void add_apc_to_thread(struct thread *t, struct apc *a,
                              enum apc_type type) {
    a->owner = t;
    apc_unset_cancelled(a);
    apc_add_tail(t, a, type);
    a->enqueued = true;
    atomic_fetch_or(&t->apc_pending_mask, apc_type_bit(type));
}

static inline bool thread_is_active(struct thread *t) {
    enum thread_state s = thread_get_state(t);
    return s == THREAD_STATE_READY || s == THREAD_STATE_RUNNING;
}

/* Poke the target core if there is no preemption on it
 * because we need the thread to reschedule to run its APC */
static void maybe_force_reschedule(struct thread *t) {
    struct scheduler *target = global.schedulers[t->last_ran];
    if (!atomic_load(&target->tick_enabled))
        scheduler_force_resched(target);
}

static void wake_if_waiting(struct thread *t) {
    if (thread_is_active(t))
        return maybe_force_reschedule(t);

    /* Get it running again */
    if (!thread_apc_sanity_check(t))
        return;

    thread_wake_manual(t);
}

void apc_enqueue(struct thread *t, struct apc *a, enum apc_type type) {
    /* avoid double-enqueue */
    if (a->enqueued)
        return;

    if (!thread_apc_sanity_check(t))
        return;

    enum irql irql = thread_acquire(t);

    add_apc_to_thread(t, a, type);

    thread_release(t, irql);
    /* Let's go and execute em */
    if (t == scheduler_get_curr_thread()) {
        thread_check_and_deliver_apcs(t);
    } else {
        /* Not us, go wake up the other guy */
        wake_if_waiting(t);
    }
}

void apc_enqueue_event_apc(struct thread *t, struct apc *a,
                           enum apc_event evt) {
    kassert(evt != APC_EVENT_NONE);
    kassert(!a->enqueued && !a->owner); /* Panic - this might be accidentally
                                         * placed on multiple threads */

    /* Only one of each type please */
    kassert(t->on_event_apcs[evt] == NULL);

    if (!thread_apc_sanity_check(t))
        return;

    enum irql irql = thread_acquire(t);

    t->on_event_apcs[evt] = a;
    a->enqueued = true;
    a->owner = t;

    thread_release(t, irql);
}

static inline void apc_unlink(struct apc *apc) {
    apc_list_del(apc);
    apc->enqueued = false;
    apc->owner = NULL;
}

static bool try_cancel_from_list(struct thread *t, struct apc *a,
                                 enum apc_type type) {
    struct list_head *pos, *n;

    list_for_each_safe(pos, n, &t->apc_head[type]) {
        struct apc *apc = apc_from_list_node(pos);
        if (apc == a) {
            apc_unlink(apc);
            return true;
        }
    }

    return false;
}

/* update pending mask if queue now empty */
static inline void update_pending_mask(struct thread *t, enum apc_type type) {
    if (apc_list_empty(t, type))
        atomic_fetch_and(&t->apc_pending_mask, ~apc_type_bit(type));
}

bool apc_cancel(struct apc *a) {
    if (!a || !a->owner)
        return false;

    struct thread *t = a->owner;
    bool removed = false;
    enum irql irql = thread_acquire(t);

    apc_set_cancelled(a);

    for (int type = 0; type < APC_TYPE_COUNT; type++) {
        removed = try_cancel_from_list(t, a, type);

        if (removed) {
            update_pending_mask(t, type);
            break;
        }
    }

    thread_release(t, irql);
    return removed;
}

void apc_enqueue_on_curr(struct apc *a, enum apc_type type) {
    apc_enqueue(scheduler_get_curr_thread(), a, type);
}

struct apc *apc_create(void) {
    return kmalloc(sizeof(struct apc));
}

void apc_init(struct apc *a, apc_func_t fn, void *arg1, void *arg2) {
    a->func = fn;
    a->arg1 = arg1;
    a->arg2 = arg2;
    INIT_LIST_HEAD(&a->node);
    a->cancelled = false;
    a->owner = NULL;
    a->enqueued = false;
}

void thread_free_event_apcs(struct thread *t) {
    for (int i = 0; i < APC_EVENT_COUNT; i++) {
        struct apc *this = t->on_event_apcs[i];
        if (this)
            kfree(this);
    }
}

void thread_set_recent_apc_event(struct thread *t, enum apc_event event) {
    t->recent_event = event;
}

void thread_exec_event_apcs(struct thread *t) {
    enum apc_event event = t->recent_event;

    if (event == APC_EVENT_NONE)
        return;

    struct apc *ea = t->on_event_apcs[event];
    if (!ea) /* No APC for this event */
        return;

    apc_execute(ea);

    t->recent_event = APC_EVENT_NONE;
}

void thread_disable_special_apcs(struct thread *t) {
    t->special_apc_disable++;
}

void thread_enable_special_apcs(struct thread *t) {
    if (--t->special_apc_disable == 0)
        thread_check_and_deliver_apcs(t);
}

void thread_disable_kernel_apcs(struct thread *t) {
    t->kernel_apc_disable++;
}

void thread_enable_kernel_apcs(struct thread *t) {
    if (--t->kernel_apc_disable == 0)
        thread_check_and_deliver_apcs(t);
}

void thread_exec_apcs(struct thread *t) {
    if (thread_can_exec_special_apcs(t))
        deliver_apc_type(t, APC_TYPE_SPECIAL_KERNEL);

    if (thread_can_exec_kernel_apcs(t))
        deliver_apc_type(t, APC_TYPE_KERNEL);

    thread_exec_event_apcs(t);
}

void thread_check_and_deliver_apcs(struct thread *t) {
    if (!t || !thread_has_apcs(t) || !safe_to_exec_apcs())
        return;

    thread_exec_apcs(t);
}
