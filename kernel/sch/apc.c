#include <mp/core.h>
#include <sch/apc.h>
#include <sch/sched.h>
#include <sch/thread.h>

#define apc_from_list_node(n) container_of(n, struct apc, node)

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

static inline bool thread_lock_apc_lock(struct thread *t) {
    return spin_lock(&t->apc_lock);
}

static inline void thread_unlock_apc_lock(struct thread *t, bool iflag) {
    spin_unlock(&t->apc_lock, iflag);
}

static inline bool thread_is_dying(struct thread *t) {
    enum thread_state s = thread_get_state(t);
    return s == THREAD_STATE_TERMINATED || s == THREAD_STATE_ZOMBIE;
}

static void thread_apc_sanity_check(struct thread *t) {
    if (unlikely(thread_get_state(t) == THREAD_STATE_IDLE_THREAD))
        k_panic("Attempted to put an APC on the idle thread");

    if (unlikely(thread_is_dying(t)))
        k_panic("Attempted to put an APC on a dying thread");
}

static inline void exec_apc(struct apc *a) {
    enum irql old = irql_raise(IRQL_APC_LEVEL);
    a->func(a, a->arg1, a->arg2);
    a->enqueued = false;
    irql_lower(old);
}

static void deliver_apc_type(struct thread *t, enum apc_type type) {
    struct apc *apc;

    while (true) {
        bool iflag = thread_lock_apc_lock(t);

        if (apc_list_empty(t, type)) {
            apc_list_unset_bitmask(t, type);
            thread_unlock_apc_lock(t, iflag);
            return;
        }

        apc = list_first_entry(&t->apc_head[type], struct apc, node);
        apc_list_del(apc);
        thread_unlock_apc_lock(t, iflag);

        apc->enqueued = false;
        if (!apc_is_cancelled(apc))
            exec_apc(apc);
    }
}

void thread_exec_apcs(struct thread *t) {
    if (thread_can_exec_special_apcs(t))
        deliver_apc_type(t, APC_TYPE_SPECIAL_KERNEL);

    if (thread_can_exec_kernel_apcs(t))
        deliver_apc_type(t, APC_TYPE_KERNEL);
}

void thread_check_and_deliver_apcs(struct thread *t) {
    if (!thread_has_apcs(t))
        return;

    if (!safe_to_exec_apcs())
        return;

    thread_exec_apcs(t);
}

static inline void add_apc_to_thread(struct thread *t, struct apc *a,
                                     enum apc_type type) {
    a->owner = t;
    apc_unset_cancelled(a);
    apc_add_tail(t, a, type);
    a->enqueued = true;
    atomic_fetch_or(&t->apc_pending_mask, apc_type_bit(type));
}

static inline bool thread_is_curr_thread(struct thread *t) {
    return t == scheduler_get_curr_thread();
}

static inline bool thread_is_active(struct thread *t) {
    enum thread_state s = thread_get_state(t);
    return s == THREAD_STATE_READY || s == THREAD_STATE_RUNNING;
}

/* Poke the target core if there is no preemption on it
 * because we need the thread to reschedule to run its APC */
static inline void maybe_force_reschedule(struct thread *t) {
    struct scheduler *target = global.schedulers[t->curr_core];
    struct core *core = global.cores[t->curr_core];
    if (!target->timeslice_enabled && core->current_irql == IRQL_PASSIVE_LEVEL)
        scheduler_force_resched(target);
}

static void wake_if_waiting(struct thread *t) {
    if (thread_is_active(t))
        return maybe_force_reschedule(t);

    /* Get it running again */
    thread_apc_sanity_check(t);
    thread_wake_manual(t);
}

void apc_enqueue(struct thread *t, struct apc *a, enum apc_type type) {
    /* avoid double-enqueue */
    if (!t || !a || a->enqueued)
        return;

    thread_apc_sanity_check(t);

    bool iflag = thread_lock_apc_lock(t);

    add_apc_to_thread(t, a, type);

    /* Let's go and execute em */
    if (thread_is_curr_thread(t)) {
        thread_check_and_deliver_apcs(t);
    } else {

        /* Not us, go wake up the other guy */
        wake_if_waiting(t);
    }

    thread_unlock_apc_lock(t, iflag);
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
    bool iflag = thread_lock_apc_lock(t);

    apc_set_cancelled(a);

    for (int type = 0; type < APC_TYPE_COUNT; type++) {
        removed = try_cancel_from_list(t, a, type);

        if (removed) {
            update_pending_mask(t, type);
            break;
        }
    }

    thread_unlock_apc_lock(t, iflag);
    return removed;
}

void apc_enqueue_on_curr(struct apc *a, enum apc_type type) {
    apc_enqueue(scheduler_get_curr_thread(), a, type);
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

void apc_init(struct apc *a, apc_func_t fn, void *arg1, void *arg2) {
    a->func = fn;
    a->arg1 = arg1;
    a->arg2 = arg2;
    INIT_LIST_HEAD(&a->node);
    a->cancelled = false;
    a->owner = NULL;
    a->enqueued = false;
}
