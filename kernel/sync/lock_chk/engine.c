#ifdef DEBUG_LOCK_CHK

#include <asm.h>
#include <atomic.h>
#include <console/panic.h>
#include <console/printf.h>
#include <enforce.h>
#include <irq/irq.h>
#include <smp/core.h>
#include <smp/percpu.h>
#include <string.h>
#include <thread/thread.h>

#include "internal.h"

PERCPU_DEFINE(uint8_t, lock_chk_recursion_depth);

struct lock_chk_guard lock_chk_enter(void) {
    struct lock_chk_guard guard = {
        .irqs_enabled = irq_disable_save(),
    };
    guard.depth = PERCPU_PTR(TOPC_IFLAG, lock_chk_recursion_depth);
    if (*guard.depth != 0)
        panic("Recursive lock validator entry");
    *guard.depth = 1;
    return guard;
}

void lock_chk_leave(const struct lock_chk_guard *guard) {
    enforce(*guard->depth == 1);
    *guard->depth = 0;
    irq_restore(guard->irqs_enabled);
}

static enum lock_chk_result
validate_acq(const struct lock_chk_acq_req *request,
             struct lock_chk_thread_data *thread_data) {
    if (request->in_nmi)
        return LOCK_CHK_RESULT_BAD_CONTEXT;

    if (lock_chk_type_is_blocking(request->lock.type)) {
        if (request->in_irq || irq_in_interrupt() ||
            request->prev_irql > IRQL_APC_LEVEL)
            return LOCK_CHK_RESULT_BAD_CONTEXT;
    } else {
        /* Spinlock / Qspinlock */
        bool irq_safe = lock_op_irq_safe(request->op_flags);
        bool raw_op = request->op_flags & LOCK_OP_RAW;
        if (!irq_safe && !raw_op) {
            if (request->in_irq || irq_in_interrupt())
                return LOCK_CHK_RESULT_BAD_CONTEXT;
        }
    }

    if (request->subclass >= LOCK_CHK_MAX_SUBCLASSES)
        return LOCK_CHK_RESULT_INTERNAL;

    if (thread_data->depth == LOCK_CHK_MAX_HELD_LOCKS)
        return LOCK_CHK_RESULT_HELD_CAPACITY;

    if (lock_chk_find_held(thread_data, request->lock.instance, 0,
                           /*match_type=*/false, LOCK_CHK_MODE_IGNORED,
                           /*match_mode=*/false) != NULL)
        return LOCK_CHK_RESULT_RECURSION;

    return LOCK_CHK_RESULT_OK;
}

void lock_chk_deep_activate(void) {
    enforce(PERCPU_READY(lock_chk_recursion_depth));
    lock_chk_graph_init(&lock_chk_global.graph);
    atomic_store_release(&lock_chk_global.deep, LOCK_CHK_ACTIVE);
}

static void lock_chk_degrade(void) {
    atomic_store_release(&lock_chk_global.deep, LOCK_CHK_DEGRADED);
}

static void lock_chk_note_capacity_exhausted(enum lock_chk_result result) {
    if ((result == LOCK_CHK_RESULT_NODE_CAPACITY ||
         result == LOCK_CHK_RESULT_EDGE_CAPACITY) &&
        !lock_chk_global.panic_on_exhaustion)
        lock_chk_degrade();
}

static void lock_chk_do_graph_fail(enum lock_chk_result result,
                                   const struct lock_chk_acq_req *request,
                                   struct lock_chk_fault *fault) {
    enforce(result != LOCK_CHK_RESULT_CYCLE &&
            result != LOCK_CHK_RESULT_EDGE_CAPACITY);

    if (result == LOCK_CHK_RESULT_BAD_CONTEXT) {
        *fault = lock_chk_fault_from_req(LOCK_CHK_FAIL_CONTEXT, request);
        lock_chk_fail(fault, "Lock class changed interrupt-safety context");
        return;
    }

    if (result == LOCK_CHK_RESULT_NODE_CAPACITY) {
        *fault = lock_chk_fault_from_req(LOCK_CHK_FAIL_CAPACITY, request);
        fault->capacity_pool = "nodes";
        fault->capacity_used = LOCK_CHK_MAX_NODES;
        fault->capacity_limit = LOCK_CHK_MAX_NODES;
        lock_chk_fail(fault, "Node capacity exhausted (%u/%u)",
                      LOCK_CHK_MAX_NODES, LOCK_CHK_MAX_NODES);
        lock_chk_note_capacity_exhausted(result);
        return;
    }

    *fault = lock_chk_fault_from_req(LOCK_CHK_FAIL_UNINITIALIZED, request);
    lock_chk_fail(fault, "Internal graph preparation failure (%u)", result);
}

static void
lock_chk_do_validation_fail(enum lock_chk_result result,
                            const struct lock_chk_acq_req *request) {
    struct lock_chk_fault fail =
        lock_chk_fault_from_req(LOCK_CHK_FAIL_UNINITIALIZED, request);

    if (result == LOCK_CHK_RESULT_BAD_CONTEXT) {
        fail.kind = LOCK_CHK_FAIL_CONTEXT;
        lock_chk_fail(&fail, "Invalid acquisition context (irq=%d, irql=%u)",
                      request->in_irq, (unsigned) request->prev_irql);
        return;
    }
    if (result == LOCK_CHK_RESULT_RECURSION) {
        fail.kind = LOCK_CHK_FAIL_RECURSION;
        lock_chk_fail(&fail, "Recursive lock acquisition (instance %p)",
                      request->lock.instance);
        return;
    }
    if (result == LOCK_CHK_RESULT_HELD_CAPACITY) {
        fail.kind = LOCK_CHK_FAIL_CAPACITY;
        fail.capacity_pool = "per-thread held";
        fail.capacity_used = LOCK_CHK_MAX_HELD_LOCKS;
        fail.capacity_limit = LOCK_CHK_MAX_HELD_LOCKS;
        lock_chk_fail(&fail, "Per-thread held capacity exhausted (%u/%u)",
                      LOCK_CHK_MAX_HELD_LOCKS, LOCK_CHK_MAX_HELD_LOCKS);
        if (!lock_chk_global.panic_on_exhaustion)
            lock_chk_degrade();
        return;
    }

    fail.kind = LOCK_CHK_FAIL_UNINITIALIZED;
    lock_chk_fail(&fail, "Subclass out of range (%u >= %u)", request->subclass,
                  LOCK_CHK_MAX_SUBCLASSES);
}

void lock_chk_before_acq(struct lock_chk_acq_token *token,
                         const struct lock_chk_acq_req *request) {
    *token = (struct lock_chk_acq_token){0};
    if (!lock_chk_state_active(&lock_chk_global.deep) ||
        request->lock.flags == LOCK_UNCHKD)
        return;

    struct lock_chk_guard guard = lock_chk_enter();

    struct thread *thread = enforce(thread_get_current());
    struct lock_chk_thread_data *thread_data = &thread->lock_chk;
    enum lock_chk_result result = validate_acq(request, thread_data);
    if (result != LOCK_CHK_RESULT_OK) {
        lock_chk_do_validation_fail(result, request);
        goto out;
    }

    struct lock_chk_node *node = NULL;
    if ((request->lock.flags & LOCK_CHKD_ORDER) != 0) {
        struct lock_chk_fault fail = {0};
        struct lock_chk_ctx ctx = {
            .graph = &lock_chk_global.graph,
            .req = request,
            .thread_data = thread_data,
            .site = request->site,
            .fault = &fail,
        };
        result = lock_chk_graph_prepare_acq(&ctx, request->map,
                                            request->subclass, &node);
        if (result != LOCK_CHK_RESULT_OK) {
            if (result == LOCK_CHK_RESULT_CYCLE ||
                result == LOCK_CHK_RESULT_EDGE_CAPACITY) {
                lock_chk_report_fail(&fail);
                lock_chk_note_capacity_exhausted(result);
            } else {
                lock_chk_do_graph_fail(result, request, &fail);
            }
            goto out;
        }
    }

    token->node = node;
    token->context_node = node;
    token->request = request;
    token->thread_data = thread_data;
    token->active = true;

out:
    lock_chk_leave(&guard);
}

void lock_chk_acqd(struct lock_chk_acq_token *token) {
    if (!token->active)
        return;

    struct lock_chk_guard guard = lock_chk_enter();

    const struct lock_chk_acq_req *request = token->request;
    struct lock_chk_thread_data *thread_data = token->thread_data;
    enforce(thread_data->depth < LOCK_CHK_MAX_HELD_LOCKS);

    thread_data->held[thread_data->depth++] = (struct lock_chk_held){
        .node = token->node,
        .lock = request->lock,
        .acquire_site = request->site,
        .acquire_tsc = rdtsc_ordered(),
        .prev_irql = request->prev_irql,
        .cpu = (uint16_t) smp_id(TOPC_IFLAG),
        .mode = request->mode,
        .subclass = request->subclass,
        .trylock = (request->op_flags & LOCK_OP_KIND_MASK) == LOCK_OP_KIND_TRY,
        .raw_operation = request->op_flags & LOCK_OP_RAW,
    };

    if ((request->lock.flags & LOCK_CHKD_THREAD) != 0) {
        thread_data->thread_checked_depth++;
        if (!(request->op_flags & LOCK_OP_RAW) &&
            lock_chk_type_is_spin(request->lock.type))
            thread_data->thread_checked_spin_depth++;
    }

    token->active = false;

    lock_chk_leave(&guard);
}

void lock_chk_cancel(struct lock_chk_acq_token *token) {
    token->active = false;
}

void lock_chk_before_rel(struct lock_chk_rel_token *token,
                         const struct lock_chk_rel_req *request) {
    *token = (struct lock_chk_rel_token){0};
    if (!lock_chk_state_active(&lock_chk_global.deep) ||
        request->lock.flags == LOCK_UNCHKD)
        return;

    struct lock_chk_guard guard = lock_chk_enter();

    struct thread *thread = enforce(thread_get_current());

    struct lock_chk_thread_data *thread_data = &thread->lock_chk;
    struct lock_chk_held *held = lock_chk_find_held(
        thread_data, request->lock.instance, request->lock.type,
        /*match_type=*/true, request->mode, /*match_mode=*/true);

    if (held != NULL) {
        token->thread_data = thread_data;
        token->instance = request->lock.instance;
        token->held_index = (uint8_t) (held - thread_data->held);
        token->active = true;
        goto out;
    }

    struct lock_chk_fault fail =
        lock_chk_fault_from_rel(LOCK_CHK_FAIL_RELEASE, request);
    lock_chk_fail(&fail, "Foreign or unbalanced lock release (instance %p)",
                  request->lock.instance);

out:
    lock_chk_leave(&guard);
}

void lock_chk_reld(struct lock_chk_rel_token *token) {
    if (!token->active)
        return;

    struct lock_chk_guard guard = lock_chk_enter();

    struct lock_chk_thread_data *thread_data = token->thread_data;
    enforce(token->held_index < thread_data->depth);
    struct lock_chk_held released = thread_data->held[token->held_index];
    enforce(released.lock.instance == token->instance);

    if ((released.lock.flags & LOCK_CHKD_THREAD) != 0) {
        enforce(thread_data->thread_checked_depth != 0);
        thread_data->thread_checked_depth--;
        if (!released.raw_operation &&
            lock_chk_type_is_spin(released.lock.type)) {
            enforce(thread_data->thread_checked_spin_depth != 0);
            thread_data->thread_checked_spin_depth--;
        }
    }

    for (uint8_t i = token->held_index; i + 1 < thread_data->depth; i++)
        thread_data->held[i] = thread_data->held[i + 1];

    thread_data->depth--;
    token->active = false;

    lock_chk_leave(&guard);
}

bool lock_chk_assert_held_deep(struct lock_chk_lock *lock,
                               enum lock_chk_mode mode, bool want_held,
                               const struct lock_chk_site *site) {
    bool mode_specific = mode != LOCK_CHK_MODE_IGNORED;
    void *instance = lock->instance;
    enum lock_chk_type type = lock->type;
    if (!lock_chk_state_active(&lock_chk_global.deep))
        return false;

    struct lock_chk_guard guard = lock_chk_enter();
    bool handled = true;

    struct thread *thread = thread_get_current();
    if (thread == NULL) {
        handled = false;
        goto out;
    }

    struct lock_chk_thread_data *thread_data = &thread->lock_chk;
    const struct lock_chk_held *held = lock_chk_find_held(
        thread_data, instance, type, /*match_type=*/true, mode, mode_specific);
    bool found = held != NULL;
    enum lock_chk_mode found_mode = found ? held->mode : mode;

    if (found != want_held) {
        struct lock_chk_fault fail = lock_chk_fault_from_lock(
            want_held ? LOCK_CHK_FAIL_NOT_HELD : LOCK_CHK_FAIL_UNEXPECTED_HELD,
            lock, site, found_mode);
        lock_chk_fail(&fail,
                      want_held ? "Lock assumed held but not held by current "
                                  "thread (instance %p)"
                                : "Lock assumed not held but held by current "
                                  "thread (instance %p)",
                      instance);
    }

out:
    lock_chk_leave(&guard);
    return handled;
}

void lock_chk_assert_schedulable(const struct lock_chk_site *site) {
    if (!lock_chk_state_active(&lock_chk_global.deep))
        return;

    struct lock_chk_guard guard = lock_chk_enter();
    struct thread *thread = thread_get_current();
    if (thread != NULL && thread->lock_chk.thread_checked_spin_depth != 0) {
        struct lock_chk_fault fail = {
            .kind = LOCK_CHK_FAIL_CONTEXT,
            .site = site,
        };
        lock_chk_fail(&fail,
                      "Scheduling while holding a thread-checked spinlock");
    }
    lock_chk_leave(&guard);
}

void lock_chk_thread_init(struct thread *thread) {
    memset(&thread->lock_chk, 0, sizeof(thread->lock_chk));
}

void lock_chk_thread_exit(struct thread *thread) {
    if (!lock_chk_state_active(&lock_chk_global.deep))
        return;

    struct lock_chk_guard guard = lock_chk_enter();
    if (thread->lock_chk.thread_checked_depth != 0) {
        struct lock_chk_fault fail = {
            .kind = LOCK_CHK_FAIL_THREAD_EXIT,
        };
        lock_chk_fail(&fail, "Thread %p exited with %u checked locks held",
                      thread, thread->lock_chk.thread_checked_depth);
    }
    lock_chk_leave(&guard);
}

#endif /* DEBUG_LOCK_CHK */
