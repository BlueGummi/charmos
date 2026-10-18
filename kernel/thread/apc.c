#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <enforce.h>
#include <math/bit.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <thread/apc.h>
#include <thread/thread.h>
#include <thread/thread_diag.h>

#include "sch/internal.h"

static inline bool safe_to_exec_apcs(void) {
    if (irql_get() != IRQL_PASSIVE_LEVEL || !irq_not_in_interrupt())
        return false;

    struct thread *curr = thread_get_current();
    if (!curr)
        return false;

    enum thread_state s = thread_get_state(curr);
    if (s == THREAD_STATE_RUNNING)
        return true;

    if ((s == THREAD_STATE_SLEEPING || s == THREAD_STATE_BLOCKED) &&
        !(thread_get_flags(curr) & THREAD_FLAG_YIELDED))
        return true;

    return false;
}

static inline enum apc_state apc_state_load(struct apc *a) {
    return atomic_load_acq(&a->state);
}

static inline bool apc_queue_empty(struct apc_queue *q) {
    return q->head == NULL;
}

static inline void apc_enqueue_tail(struct apc_queue *q, struct apc *a) {
    a->next = NULL;

    if (!q->head) {
        q->head = q->tail = a;
    } else {
        q->tail->next = a;
        q->tail = a;
    }
}

static inline struct apc *apc_dequeue_head(struct apc_queue *q) {
    struct apc *a = q->head;
    if (!a)
        return NULL;

    q->head = a->next;
    if (!q->head)
        q->tail = NULL;

    a->next = NULL;
    return a;
}

static inline void apc_add_tail(struct thread *t, struct apc *a,
                                enum apc_type type) {
    apc_enqueue_tail(&t->apc_head[type], a);
}

static inline bool apc_list_empty(struct thread *t, enum apc_type type) {
    return apc_queue_empty(&t->apc_head[type]);
}

static inline void apc_unset_bitmask(struct thread *t, enum apc_type type) {
    atomic_clear_bit(&t->apc_pending_mask, type);
}

static inline void apc_set_bitmask(struct thread *t, enum apc_type type) {
    atomic_set_bit(&t->apc_pending_mask, type);
}

static inline bool thread_can_exec_special_apcs(struct thread *t) {
    return t->special_apc_disable == 0 &&
           atomic_test_bit(&t->apc_pending_mask, APC_TYPE_SPECIAL_KERNEL);
}

static inline bool thread_can_exec_kernel_apcs(struct thread *t) {
    return t->kernel_apc_disable == 0 &&
           atomic_test_bit(&t->apc_pending_mask, APC_TYPE_KERNEL);
}

static inline bool thread_is_dying(struct thread *t) {
    if (thread_get_flags(t) & THREAD_FLAG_DYING)
        return true;
    enum thread_state s = thread_get_state(t);
    return s == THREAD_STATE_TERMINATED || s == THREAD_STATE_ZOMBIE;
}

static bool thread_apc_sanity_check(struct thread *t) {
    if (cc_unlikely(thread_get_state(t) == THREAD_STATE_IDLE_THREAD))
        panic("Attempted to put an APC on the idle thread");

    if (cc_unlikely(thread_is_dying(t)))
        return false;

    return true;
}

static void apc_execute(struct apc *a) {
    enforce(irql_get() == IRQL_APC_LEVEL);

    struct thread *curr = thread_get_current();

    thread_or_flags(curr, THREAD_FLAG_EXECUTING_APC);

    struct apc_context ac = {.apc = a};
    a->func(&ac, a->ctx);

    thread_and_flags(curr, ~THREAD_FLAG_EXECUTING_APC);

#ifdef DEBUG_THREAD
    curr->total_apcs_ran++;
#endif
}

/* TODO: Work on this budget thing */
#define APC_DELIVER_BUDGET 32

static void deliver_apc_type(struct thread *t, enum apc_type type) {
    uint32_t budget = APC_DELIVER_BUDGET;
    uint64_t drained = 0;

    while (budget--) {
        thread_diag_apc_deliver_batch(t, drained);

        bool ok;
        enum irql irql = thread_acquire(t, &ok);
        if (!ok)
            return;

        struct apc *apc = apc_dequeue_head(&t->apc_head[type]);

        if (!apc) {
            apc_unset_bitmask(t, type);
            thread_release(t, irql);
            return;
        }

        enforce(apc->owner == t);
        enforce(apc_state_load(apc) == APC_STATE_QUEUED);
        apc->owner = NULL;
        atomic_store_release(&apc->state, APC_STATE_EXECUTING);

        thread_release(t, irql);

        apc_execute(apc);
        atomic_store_release(&apc->state, APC_STATE_IDLE);
        apc_putref(apc);
        drained++;
    }
}

static void add_apc_to_thread(struct thread *t, struct apc *a,
                              enum apc_type type) {
    a->owner = t;
    apc_add_tail(t, a, type);
    apc_set_bitmask(t, type);
}

static void wake_if_waiting(struct thread *t) {
    if (thread_apc_sanity_check(t))
        scheduler_resume_for_apc(t);
}

bool apc_enqueue(struct thread *t, struct apc *a, enum apc_type type) {
    if (!t || !a || type >= APC_TYPE_COUNT || !thread_apc_sanity_check(t))
        return false;

    bool ok;
    enum irql irql = thread_acquire(t, &ok);
    if (!ok)
        return false;

    if (thread_is_dying(t)) {
        thread_release(t, irql);
        return false;
    }

    if (!apc_getref(a)) {
        thread_release(t, irql);
        return false;
    }

    enum apc_state expected = APC_STATE_IDLE;
    if (!atomic_cas_strong(&a->state, &expected, APC_STATE_QUEUED, mo_acq_rel,
                           mo_acquire)) {
        thread_release(t, irql);
        apc_putref(a);
        return false;
    }

    enforce(a->owner == NULL);
    add_apc_to_thread(t, a, type);
    thread_release(t, irql);

    /* Let's go and execute em */
    if (t == thread_get_current()) {
        apc_check_and_deliver(t);
    } else {
        /* Not us, go wake up the other guy */
        wake_if_waiting(t);
    }
    return true;
}

static bool try_cancel_from_queue(struct thread *t, struct apc *a,
                                  enum apc_type type) {
    struct apc_queue *q = &t->apc_head[type];

    struct apc *prev = NULL;
    struct apc *curr = q->head;

    while (curr) {
        struct apc *next = curr->next;

        if (curr == a) {
            enforce(curr->owner == t);
            enforce(apc_state_load(curr) == APC_STATE_QUEUED);
            if (prev)
                prev->next = next;
            else
                q->head = next;

            if (q->tail == curr)
                q->tail = prev;

            curr->next = NULL;
            curr->owner = NULL;

            return true;
        }

        prev = curr;
        curr = next;
    }

    return false;
}

/* update pending mask if queue now empty */
static inline void update_pending_mask(struct thread *t, enum apc_type type) {
    if (apc_list_empty(t, type))
        atomic_clear_bit(&t->apc_pending_mask, type);
}

bool apc_cancel(struct thread *t, struct apc *a) {
    if (!t || !a)
        return false;

    bool removed = false;
    bool ok;
    enum irql irql = thread_acquire(t, &ok);
    if (!ok)
        return false;

    for (int type = 0; type < APC_TYPE_COUNT; type++) {
        removed = try_cancel_from_queue(t, a, type);

        if (removed) {
            update_pending_mask(t, type);
            break;
        }
    }

    thread_release(t, irql);
    if (removed) {
        atomic_store_release(&a->state, APC_STATE_IDLE);
        apc_putref(a);
    }
    return removed;
}

struct apc *apc_create(void) {
    struct apc *a = vnew(a);
    return a;
}

void apc_init(struct apc *a, apc_func_t fn, void *arg1, apc_destroy_t destroy) {
    enforce(a);
    enforce(fn);
    a->func = fn;
    a->ctx = arg1;
    a->next = NULL;
    a->owner = NULL;
    refcount_init(&a->refcount, 1);
    atomic_store_rlx(&a->state, APC_STATE_IDLE);
    a->destroy = destroy;
}

bool apc_getref(struct apc *a) {
    return a && refcount_inc_not_zero(&a->refcount);
}

void apc_putref(struct apc *a) {
    enforce(a);
    if (!refcount_dec_and_test(&a->refcount))
        return;

    enforce(apc_state_load(a) == APC_STATE_IDLE);
    enforce(a->owner == NULL);
    enforce(a->next == NULL);
    if (a->destroy)
        a->destroy(a);
}

void apc_destroy_free(struct apc *a) {
    vfree(a);
}

static void apc_queue_splice(struct apc_queue *from, struct apc_queue *to) {
    struct apc *a;

    while ((a = apc_dequeue_head(from)))
        apc_enqueue_tail(to, a);
}

void apc_rundown_thread(struct thread *t) {
    struct apc_queue drained = {0};

    /* Every other queue mutator will hold t->lock,
     * and rundown has to do that too */
    enum irql irql = spin_lock_high(&t->lock);

    for (size_t type = 0; type < APC_TYPE_COUNT; type++)
        apc_queue_splice(&t->apc_head[type], &drained);

    atomic_store_release(&t->apc_pending_mask, 0);

    spin_unlock(&t->lock, irql);

    /* We can't run the teardown from the critical section */
    struct apc *a;
    while ((a = apc_dequeue_head(&drained))) {
        a->owner = NULL;
        atomic_store_release(&a->state, APC_STATE_IDLE);
        apc_putref(a);
    }
}

static void apc_execute_event(struct apc *a) {
    enforce(apc_state_load(a) == APC_STATE_QUEUED);
    atomic_store_release(&a->state, APC_STATE_EXECUTING);
    apc_execute(a);
    atomic_store_release(&a->state, APC_STATE_QUEUED);
}

void apc_disable_special() {
    thread_get_current()->special_apc_disable++;
}

void apc_enable_special() {
    struct thread *t = thread_get_current();
    enforce(t->special_apc_disable > 0);

    if (--t->special_apc_disable == 0)
        apc_check_and_deliver(t);
}

void apc_disable_kernel() {
    thread_get_current()->kernel_apc_disable++;
}

void apc_enable_kernel() {
    struct thread *t = thread_get_current();
    enforce(t->kernel_apc_disable > 0);

    if (--t->kernel_apc_disable == 0)
        apc_check_and_deliver(t);
}

static inline bool thread_can_exec_any_apcs(struct thread *t) {
    return thread_can_exec_special_apcs(t) || thread_can_exec_kernel_apcs(t);
}

void thread_exec_apcs(struct thread *t) {
    if (thread_can_exec_special_apcs(t))
        deliver_apc_type(t, APC_TYPE_SPECIAL_KERNEL);

    if (thread_can_exec_kernel_apcs(t))
        deliver_apc_type(t, APC_TYPE_KERNEL);
}

void apc_check_and_deliver(struct thread *t) {
    if (!t || !safe_to_exec_apcs() || !thread_can_exec_any_apcs(t))
        return;

    if (thread_get_flags(t) & (THREAD_FLAG_EXECUTING_APC |
                               THREAD_FLAG_DELIVERING_APCS | THREAD_FLAG_DYING))
        return;

    /* Which IRQL-lowering site keeps handing this thread back to delivery */
    thread_diag_apc_deliver_enter(t, ci_return_address(0));

    thread_or_flags(t, THREAD_FLAG_DELIVERING_APCS);

    enum irql irql = irql_raise(IRQL_APC_LEVEL);

    thread_exec_apcs(t);

    irql_lower(irql);

    thread_and_flags(t, ~THREAD_FLAG_DELIVERING_APCS);
}
