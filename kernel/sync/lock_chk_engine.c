#ifdef DEBUG_LOCK_CHK

#include <asm.h>
#include <console/panic.h>
#include <console/printf.h>
#include <irq/irq.h>
#include <kassert.h>
#include <smp/core.h>
#include <smp/percpu.h>
#include <stdatomic.h>
#include <string.h>
#include <thread/thread.h>

#include "lock_chk_internal.h"

static struct lock_chk_graph lock_chk_global_graph;
static _Atomic enum lock_chk_engine_state lock_chk_deep_state =
    LOCK_CHK_INACTIVE;
PERCPU_DECLARE(lock_chk_recursion_depth, uint8_t, NULL);

struct lock_chk_guard {
    uint8_t *depth;
    bool irqs_enabled;
};

static struct lock_chk_guard lock_chk_enter(void) {
    struct lock_chk_guard guard = {
        .irqs_enabled = are_interrupts_enabled(),
    };
    disable_interrupts();
    guard.depth = PERCPU_PTR(lock_chk_recursion_depth);
    if (*guard.depth != 0)
        panic("Recursive lock validator entry");
    *guard.depth = 1;
    return guard;
}

static void lock_chk_leave(const struct lock_chk_guard *guard) {
    kassert(*guard->depth == 1);
    *guard->depth = 0;
    if (guard->irqs_enabled)
        enable_interrupts();
}

static bool lock_chk_deep_is_active(void) {
    return atomic_load_explicit(&lock_chk_deep_state, memory_order_acquire) ==
           LOCK_CHK_ACTIVE;
}

static enum lock_chk_result
lock_chk_validate_acquire(const struct lock_chk_acquire_request *request,
                          struct lock_chk_thread_data *thread_data) {
    if (request->type == LOCK_CHK_TYPE_MUTEX ||
        request->type == LOCK_CHK_TYPE_MUTEX_SIMPLE ||
        request->type == LOCK_CHK_TYPE_RWLOCK) {
        if (request->in_nmi || request->in_irq || !irq_in_thread_context() ||
            request->prev_irql > IRQL_APC_LEVEL)
            return LOCK_CHK_RESULT_BAD_CONTEXT;
    } else {
        /* Spinlock / Qspinlock */
        if (request->in_nmi)
            return LOCK_CHK_RESULT_BAD_CONTEXT;

        if (!request->irq_safe && !request->raw_operation) {
            if (request->in_irq || !irq_in_thread_context())
                return LOCK_CHK_RESULT_BAD_CONTEXT;
        }
    }

    if (request->subclass >= LOCK_CHK_MAX_SUBCLASSES)
        return LOCK_CHK_RESULT_INTERNAL;

    if (thread_data->depth == LOCK_CHK_MAX_HELD_LOCKS)
        return LOCK_CHK_RESULT_HELD_CAPACITY;

    for (uint8_t i = 0; i < thread_data->depth; i++)
        if (thread_data->held[i].instance == request->instance)
            return LOCK_CHK_RESULT_RECURSION;

    return LOCK_CHK_RESULT_OK;
}

void lock_chk_deep_activate(void) {
    kassert(PERCPU_READY(lock_chk_recursion_depth));
    lock_chk_graph_init(&lock_chk_global_graph);
    atomic_store_explicit(&lock_chk_deep_state, LOCK_CHK_ACTIVE,
                          memory_order_release);
}

static void lock_chk_degrade(void) {
    atomic_store_explicit(&lock_chk_deep_state, LOCK_CHK_DEGRADED,
                          memory_order_release);
}

static void
lock_chk_handle_graph_failure(enum lock_chk_result result,
                              const struct lock_chk_acquire_request *request,
                              struct lock_chk_failure *failure) {
    if (result == LOCK_CHK_RESULT_BAD_CONTEXT) {
        *failure = (struct lock_chk_failure){
            .kind = LOCK_CHK_FAIL_CONTEXT,
            .site = request->site,
            .class = request->map ? request->map->class : NULL,
            .instance = request->instance,
            .type = request->type,
            .mode = request->mode,
            .subclass = request->subclass,
        };
        lock_chk_fail(failure, "Lock class changed interrupt-safety context");
        return;
    }

    if (result == LOCK_CHK_RESULT_NODE_CAPACITY) {
        *failure = (struct lock_chk_failure){
            .kind = LOCK_CHK_FAIL_CAPACITY,
            .site = request->site,
            .class = request->map ? request->map->class : NULL,
            .instance = request->instance,
            .type = request->type,
            .mode = request->mode,
            .subclass = request->subclass,
            .capacity_pool = "nodes",
            .capacity_used = LOCK_CHK_MAX_NODES,
            .capacity_limit = LOCK_CHK_MAX_NODES,
        };
        lock_chk_fail(failure, "Node capacity exhausted (%u/%u)",
                      LOCK_CHK_MAX_NODES, LOCK_CHK_MAX_NODES);
    } else if (result == LOCK_CHK_RESULT_CYCLE ||
               result == LOCK_CHK_RESULT_EDGE_CAPACITY) {
        lock_chk_report_failure(failure);
    } else {
        *failure = (struct lock_chk_failure){
            .kind = LOCK_CHK_FAIL_UNINITIALIZED,
            .site = request->site,
            .instance = request->instance,
            .type = request->type,
            .mode = request->mode,
            .subclass = request->subclass,
        };
        lock_chk_fail(failure, "Internal graph preparation failure (%u)",
                      (unsigned) result);
        return;
    }

    if ((result == LOCK_CHK_RESULT_NODE_CAPACITY ||
         result == LOCK_CHK_RESULT_EDGE_CAPACITY) &&
        !lock_chk_capacity_should_panic())
        lock_chk_degrade();
}

static void lock_chk_handle_validation_failure(
    enum lock_chk_result result,
    const struct lock_chk_acquire_request *request) {
    struct lock_chk_failure fail = {
        .site = request->site,
        .class = request->map ? request->map->class : NULL,
        .instance = request->instance,
        .type = request->type,
        .mode = request->mode,
        .subclass = request->subclass,
    };

    if (result == LOCK_CHK_RESULT_BAD_CONTEXT) {
        fail.kind = LOCK_CHK_FAIL_CONTEXT;
        lock_chk_fail(
            &fail, "Invalid acquisition context (nmi=%d, irq=%d, irql=%u)",
            request->in_nmi, request->in_irq, (unsigned) request->prev_irql);
        return;
    }
    if (result == LOCK_CHK_RESULT_RECURSION) {
        fail.kind = LOCK_CHK_FAIL_RECURSION;
        lock_chk_fail(&fail, "Recursive lock acquisition (instance %p)",
                      request->instance);
        return;
    }
    if (result == LOCK_CHK_RESULT_HELD_CAPACITY) {
        fail.kind = LOCK_CHK_FAIL_CAPACITY;
        fail.capacity_pool = "per-thread held";
        fail.capacity_used = LOCK_CHK_MAX_HELD_LOCKS;
        fail.capacity_limit = LOCK_CHK_MAX_HELD_LOCKS;
        lock_chk_fail(&fail, "Per-thread held capacity exhausted (%u/%u)",
                      LOCK_CHK_MAX_HELD_LOCKS, LOCK_CHK_MAX_HELD_LOCKS);
        if (!lock_chk_capacity_should_panic())
            lock_chk_degrade();
        return;
    }

    fail.kind = LOCK_CHK_FAIL_UNINITIALIZED;
    lock_chk_fail(&fail, "Subclass out of range (%u >= %u)", request->subclass,
                  LOCK_CHK_MAX_SUBCLASSES);
}

void lock_chk_before_acquire(struct lock_chk_acquire_token *token,
                             const struct lock_chk_acquire_request *request) {
    *token = (struct lock_chk_acquire_token){0};
    if (!lock_chk_deep_is_active() || request->flags == LOCK_UNCHKD)
        return;

    struct lock_chk_guard guard = lock_chk_enter();

    struct thread *thread = thread_get_current();
    if (thread == NULL) {
        struct lock_chk_failure fail = {
            .kind = LOCK_CHK_FAIL_CONTEXT,
            .site = request->site,
            .type = request->type,
            .mode = request->mode,
            .subclass = request->subclass,
        };
        lock_chk_fail(&fail, "Checked acquisition without a current thread");
        goto out;
    }

    struct lock_chk_thread_data *thread_data = &thread->lock_chk;
    enum lock_chk_result result =
        lock_chk_validate_acquire(request, thread_data);
    if (result != LOCK_CHK_RESULT_OK) {
        lock_chk_handle_validation_failure(result, request);
        goto out;
    }

    struct lock_chk_node *node = NULL;
    if ((request->flags & LOCK_CHKD_ORDER) != 0) {
        struct lock_chk_failure fail = {0};
        result = lock_chk_graph_prepare_acquire(
            &lock_chk_global_graph, request->map, request->subclass, request,
            thread_data, &node, &fail);
        if (result != LOCK_CHK_RESULT_OK) {
            lock_chk_handle_graph_failure(result, request, &fail);
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

void lock_chk_acquired(struct lock_chk_acquire_token *token) {
    if (!token->active)
        return;

    struct lock_chk_guard guard = lock_chk_enter();

    const struct lock_chk_acquire_request *request = token->request;
    struct lock_chk_thread_data *thread_data = token->thread_data;
    kassert(thread_data->depth < LOCK_CHK_MAX_HELD_LOCKS);

    thread_data->held[thread_data->depth++] = (struct lock_chk_held){
        .node = token->node,
        .instance = request->instance,
        .acquire_site = request->site,
        .acquire_tsc = rdtsc_ordered(),
        .prev_irql = request->prev_irql,
        .cpu = smp_core_id(),
        .flags = request->flags,
        .type = request->type,
        .mode = request->mode,
        .subclass = request->subclass,
        .trylock = request->wait_kind == LOCK_CHK_WAIT_TRY,
        .raw_operation = request->raw_operation,
    };

    if ((request->flags & LOCK_CHKD_THREAD) != 0) {
        thread_data->thread_checked_depth++;
        if (!request->raw_operation && (request->type == LOCK_CHK_TYPE_SPIN ||
                                        request->type == LOCK_CHK_TYPE_QSPIN))
            thread_data->thread_checked_spin_depth++;
    }

    token->active = false;

    lock_chk_leave(&guard);
}

void lock_chk_cancel(struct lock_chk_acquire_token *token) {
    token->active = false;
}

void lock_chk_before_release(struct lock_chk_release_token *token,
                             const struct lock_chk_release_request *request) {
    *token = (struct lock_chk_release_token){0};
    if (!lock_chk_deep_is_active() || request->flags == LOCK_UNCHKD)
        return;

    struct lock_chk_guard guard = lock_chk_enter();

    struct thread *thread = thread_get_current();
    if (thread == NULL) {
        struct lock_chk_failure fail = {
            .kind = LOCK_CHK_FAIL_CONTEXT,
            .site = request->site,
            .type = request->type,
            .mode = request->mode,
        };
        lock_chk_fail(&fail, "Checked release without a current thread");
        goto out;
    }

    struct lock_chk_thread_data *thread_data = &thread->lock_chk;
    for (uint8_t i = 0; i < thread_data->depth; i++) {
        struct lock_chk_held *held = &thread_data->held[i];
        if (held->instance != request->instance ||
            held->type != request->type || held->mode != request->mode)
            continue;

        token->thread_data = thread_data;
        token->instance = request->instance;
        token->held_index = i;
        token->active = true;
        goto out;
    }

    {
        struct lock_chk_failure fail = {
            .kind = LOCK_CHK_FAIL_RELEASE,
            .site = request->site,
            .class = request->map ? request->map->class : NULL,
            .instance = request->instance,
            .type = request->type,
            .mode = request->mode,
        };
        lock_chk_fail(&fail, "Foreign or unbalanced lock release (instance %p)",
                      request->instance);
    }

out:
    lock_chk_leave(&guard);
}

void lock_chk_released(struct lock_chk_release_token *token) {
    if (!token->active)
        return;

    struct lock_chk_guard guard = lock_chk_enter();

    struct lock_chk_thread_data *thread_data = token->thread_data;
    kassert(token->held_index < thread_data->depth);
    struct lock_chk_held released = thread_data->held[token->held_index];
    kassert(released.instance == token->instance);

    if ((released.flags & LOCK_CHKD_THREAD) != 0) {
        kassert(thread_data->thread_checked_depth != 0);
        thread_data->thread_checked_depth--;
        if (!released.raw_operation && (released.type == LOCK_CHK_TYPE_SPIN ||
                                        released.type == LOCK_CHK_TYPE_QSPIN)) {
            kassert(thread_data->thread_checked_spin_depth != 0);
            thread_data->thread_checked_spin_depth--;
        }
    }

    for (uint8_t i = token->held_index; i + 1 < thread_data->depth; i++)
        thread_data->held[i] = thread_data->held[i + 1];

    thread_data->depth--;
    token->active = false;

    lock_chk_leave(&guard);
}

bool lock_chk_assert_held_deep(struct lock_chk_map *map, void *instance,
                               enum lock_chk_type type, enum lock_chk_mode mode,
                               bool mode_specific, bool want_held,
                               const struct lock_chk_site *site) {
    if (!lock_chk_deep_is_active())
        return false;

    struct lock_chk_guard guard = lock_chk_enter();
    bool handled = true;

    struct thread *thread = thread_get_current();
    if (thread == NULL) {
        handled = false;
        goto out;
    }

    struct lock_chk_thread_data *thread_data = &thread->lock_chk;
    bool found = false;
    enum lock_chk_mode found_mode = mode;
    for (uint8_t i = 0; i < thread_data->depth; i++) {
        struct lock_chk_held *held = &thread_data->held[i];
        if (held->instance != instance || held->type != type)
            continue;
        if (mode_specific && held->mode != mode)
            continue;
        found = true;
        found_mode = held->mode;
        break;
    }

    if (found != want_held) {
        struct lock_chk_failure fail = {
            .kind = want_held ? LOCK_CHK_FAIL_NOT_HELD
                              : LOCK_CHK_FAIL_UNEXPECTED_HELD,
            .site = site,
            .class = map ? map->class : NULL,
            .instance = instance,
            .type = type,
            .mode = found_mode,
        };
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
    if (!lock_chk_deep_is_active())
        return;

    struct lock_chk_guard guard = lock_chk_enter();
    struct thread *thread = thread_get_current();
    if (thread != NULL && thread->lock_chk.thread_checked_spin_depth != 0) {
        struct lock_chk_failure fail = {
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
    if (!lock_chk_deep_is_active())
        return;

    struct lock_chk_guard guard = lock_chk_enter();
    if (thread->lock_chk.thread_checked_depth != 0) {
        struct lock_chk_failure fail = {
            .kind = LOCK_CHK_FAIL_THREAD_EXIT,
        };
        lock_chk_fail(&fail, "Thread %p exited with %u checked locks held",
                      thread, thread->lock_chk.thread_checked_depth);
    }
    lock_chk_leave(&guard);
}

#endif /* DEBUG_LOCK_CHK */
