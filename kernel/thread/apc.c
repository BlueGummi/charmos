#include <kassert.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <thread/apc.h>
#include <thread/thread.h>

#include "sch/internal.h"

#define apc_from_list_node(n) container_of(n, struct apc, list)

static inline bool safe_to_exec_apcs(void) {
    return irql_get() == IRQL_PASSIVE_LEVEL && irq_in_thread_context();
}

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
    list_del_init(&a->list);
}

static inline void apc_add_tail(struct thread *t, struct apc *a,
                                enum apc_type type) {
    list_add_tail(&a->list, &t->apc_head[type]);
}

static inline void apc_unset_bitmask(struct thread *t, enum apc_type type) {
    atomic_fetch_and(&t->apc_pending_mask, ~apc_type_bit(type));
}

static inline void apc_set_bitmask(struct thread *t, enum apc_type type) {
    atomic_fetch_or(&t->apc_pending_mask, apc_type_bit(type));
}

static inline void apc_set_cancelled(struct apc *a, bool new) {
    atomic_store(&a->cancelled, new);
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
        panic("Attempted to put an APC on the idle thread");

    if (unlikely(thread_is_dying(t)))
        return false;

    return true;
}

static void apc_execute(struct apc *a) {
    kassert(irql_get() == IRQL_APC_LEVEL);

    struct thread *curr = thread_get_current();

    thread_or_flags(curr, THREAD_FLAG_EXECUTING_APC);

    a->func(a, a->arg1, a->arg2);

    thread_and_flags(curr, ~THREAD_FLAG_EXECUTING_APC);
    curr->total_apcs_ran++;
}

static void deliver_apc_type(struct thread *t, enum apc_type type) {
    struct apc *apc;

    while (true) {
        bool ok;
        enum irql irql = thread_acquire(t, &ok);
        kassert(ok); /* should not fail */

        if (apc_list_empty(t, type)) {
            apc_unset_bitmask(t, type);
            thread_release(t, irql);
            return;
        }

        apc = list_first_entry(&t->apc_head[type], struct apc, list);
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
    apc_set_cancelled(a, false);
    apc_add_tail(t, a, type);
    a->enqueued = true;
    apc_set_bitmask(t, type);
}

static inline bool thread_is_active(struct thread *t) {
    enum thread_state s = thread_get_state(t);
    return s == THREAD_STATE_READY || s == THREAD_STATE_RUNNING;
}

static void maybe_force_resched(struct thread *t) {
    /* it's ok if the read of tick_enabled races here. if we read it as
     * `enabled`, it means that it is either truly enabled or is in
     * the schedule() routine about to disable it, meaning that
     * if it does get disabled, it'll still have a chance to check
     * and run the APCs of the only thread active */
    enum irql irql;
    struct scheduler *sched = thread_get_scheduler(t, &irql);

    bool needs_resched = !sched->tick_enabled;
    if (needs_resched)
        scheduler_force_resched(sched);

    spin_unlock(&sched->lock, irql);
}

static void wake_if_waiting(struct thread *t) {
    if (thread_is_active(t))
        maybe_force_resched(t);

    /* Get it running again */
    if (!thread_apc_sanity_check(t))
        return;

    /* set the wake_src as the thread that enqueued the APC */
    thread_wake_manual(t, /* wake_src = */ t);
}

void apc_enqueue(struct thread *t, struct apc *a, enum apc_type type) {
    if (!thread_apc_sanity_check(t))
        return;

    bool ok;
    enum irql irql = thread_acquire(t, &ok);
    if (!ok)
        return;

    if (a->enqueued) {
        thread_release(t, irql);
        return;
    }

    add_apc_to_thread(t, a, type);
    thread_release(t, irql);

    /* Let's go and execute em */
    if (t == thread_get_current()) {
        apc_check_and_deliver(t);
    } else {
        /* Not us, go wake up the other guy */
        wake_if_waiting(t);
    }
}

/* We can only enqueue and run from ourselves, no sync needed */
void apc_enqueue_event_apc(struct apc *a, struct apc_event_desc *desc) {
    kassert(desc && "cannot have a NULL APC event descriptor");
    kassert(list_empty(&a->list));
    kassert(!a->enqueued && !a->owner); /* Panic - this might be accidentally
                                         * placed on multiple threads */

    a->desc = desc;
    struct thread *t = thread_get_current();
    if (!thread_apc_sanity_check(t))
        return;

    list_add_tail(&a->list, &t->event_apcs);

    a->enqueued = true;
    a->owner = t;
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
    bool ok;
    enum irql irql = thread_acquire(t, &ok);
    if (!ok)
        return false;

    apc_set_cancelled(a, true);

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

struct apc *apc_create(void) {
    return kmalloc(sizeof(struct apc), ALLOC_PARAMS_DEFAULT);
}

void apc_init(struct apc *a, apc_func_t fn, void *arg1, void *arg2) {
    a->func = fn;
    a->arg1 = arg1;
    a->arg2 = arg2;
    INIT_LIST_HEAD(&a->list);
    a->cancelled = false;
    a->owner = NULL;
    a->enqueued = false;
}

void apc_free_on_thread(struct thread *t) {
    struct apc *a, *tmp;
    kassert(list_empty(&t->to_exec_event_apcs));

    list_for_each_entry_safe(a, tmp, &t->event_apcs, list) {
        list_del(&a->list);
        kfree(a, FREE_PARAMS_DEFAULT);
    }
}

static void bump_counters_on_list(struct list_head *from,
                                  struct apc_event_desc *desc,
                                  struct list_head *to) {
    struct apc *a, *tmp;
    list_for_each_entry_safe(a, tmp, from, list) {
        if (a->desc != desc)
            continue;

        a->execute_times++;
        if (to) {
            list_del_init(&a->list);
            list_add_tail(&a->list, to);
        }
    }
}

void apc_event_signal(struct apc_event_desc *desc) {
    /* here we want to do two things: first, we identify if it is safe to
     * execute APCs. if it is, it must be guaranteed that the to_execute
     * tree of event APCs is empty, because the irql_lower that should've
     * happened would have executed anything on that tree. in this case,
     * we check our event_apcs tree, and execute anything of relevance
     * in there. if it is not safe to execute APCs, we will check
     * the to_execute tree, increment counters for all relevant APCs, and then
     * check the event_apcs tree, and move anything necessary over */
    struct thread *curr = thread_get_current();

    if (safe_to_exec_apcs() && curr->kernel_apc_disable == 0) {
        kassert(list_empty(&curr->to_exec_event_apcs));
        enum irql irql = irql_raise(IRQL_APC_LEVEL);

        /* This will give us the "first node in a list" that matches our `desc`
         * value. We can keep going this->right->right to find everyone else to
         * execute */
        struct apc *a;
        list_for_each_entry(a, &curr->event_apcs, list) {
            if (a->desc == desc)
                apc_execute(a);
        }

        irql_lower(irql);
    } else {
        /* Cannot execute APCs right now. Search both trees, bump counters. */
        bump_counters_on_list(&curr->to_exec_event_apcs, desc, NULL);
        bump_counters_on_list(&curr->event_apcs, desc,
                              &curr->to_exec_event_apcs);

        apc_set_bitmask(curr, APC_TYPE_KERNEL);
    }
}

void thread_exec_event_apcs(struct thread *t) {
    struct apc *a, *tmp;
    list_for_each_entry_safe(a, tmp, &t->to_exec_event_apcs, list) {
        kassert(a->execute_times);
        for (size_t i = 0; i < a->execute_times; i++)
            apc_execute(a);

        a->execute_times = 0;
        list_del_init(&a->list);
        list_add_tail(&a->list, &t->event_apcs);
    }

    /* Just in case */
    apc_unset_bitmask(t, APC_TYPE_KERNEL);
}

void apc_disable_special() {
    thread_get_current()->special_apc_disable++;
}

void apc_enable_special() {
    struct thread *t = thread_get_current();
    kassert(t->special_apc_disable > 0);

    if (--t->special_apc_disable == 0)
        apc_check_and_deliver(t);
}

void apc_disable_kernel() {
    thread_get_current()->kernel_apc_disable++;
}

void apc_enable_kernel() {
    struct thread *t = thread_get_current();
    kassert(t->kernel_apc_disable > 0);

    if (--t->kernel_apc_disable == 0)
        apc_check_and_deliver(t);
}

void thread_exec_apcs(struct thread *t) {
    if (thread_can_exec_special_apcs(t))
        deliver_apc_type(t, APC_TYPE_SPECIAL_KERNEL);

    if (thread_can_exec_kernel_apcs(t)) {
        deliver_apc_type(t, APC_TYPE_KERNEL);
        thread_exec_event_apcs(t);
    }
}

void apc_check_and_deliver(struct thread *t) {
    if (!t || !thread_has_apcs(t) || !safe_to_exec_apcs())
        return;

    enum irql irql = irql_raise(IRQL_APC_LEVEL);

    thread_exec_apcs(t);

    irql_lower(irql);
}
