#include <acpi/lapic.h>
#include <kassert.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <stdint.h>
#include <thread/dpc.h>

static struct dpc *dpc_steal_queue(struct dpc_queue *dq) {
    struct dpc *list =
        atomic_exchange_explicit(&dq->head, NULL, memory_order_acquire);

    if (!list)
        return NULL;

    struct dpc *rev = NULL;

    while (list) {
        struct dpc *next =
            atomic_load_explicit(&list->next, memory_order_relaxed);
        atomic_store_explicit(&list->next, rev, memory_order_relaxed);
        rev = list;
        list = next;
    }

    return rev;
}

static void dpc_execute_all_in_queue(struct dpc_queue *dq) {
    while (true) {
        struct dpc *it = dpc_steal_queue(dq);
        if (!it)
            break;

        while (it) {
            struct dpc *next =
                atomic_load_explicit(&it->next, memory_order_relaxed);
            atomic_store_explicit(&it->enqueued, false, memory_order_release);
            it->func(it->ctx);
            it = next;
        }
    }
}

void dpc_drain_local(void) {
    struct core *me = smp_core();
    if (me->in_resched)
        return;

    /* Recursion guard */
    if (atomic_exchange(&me->executing_dpcs, true))
        return;

    kassert(irql_get() == IRQL_DISPATCH_LEVEL);

    size_t cpu = me->id;
    struct dpc_cpu *dc = &global.dpc_data[cpu];

    do {
        dpc_execute_all_in_queue(&dc->queue);
    } while (atomic_load_explicit(&dc->queue.head, memory_order_relaxed) !=
             NULL);

    atomic_store(&me->executing_dpcs, false);
}

void dpc_run_local(void) {
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    dpc_drain_local();
    irql_lower(irql);
}

void dpc_run_dpcs_from_irq(void) {
    dpc_run_local();
}

static void dpc_queue_enqueue(struct dpc_queue *dq, struct dpc *d) {
    while (true) {
        struct dpc *old_head =
            atomic_load_explicit(&dq->head, memory_order_acquire);
        atomic_store_explicit(&d->next, old_head, memory_order_relaxed);
        if (atomic_compare_exchange_weak_explicit(&dq->head, &old_head, d,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            break;
        }
        cpu_relax();
    }
}

bool dpc_enqueue_on_cpu(size_t cpu, struct dpc *d) {
    kassert(d);

    if (atomic_exchange_explicit(&d->enqueued, true, memory_order_acq_rel))
        return false;

    struct dpc_cpu *dc = &global.dpc_data[cpu];

    /* Clear next pointer then push via CAS loop */
    atomic_store_explicit(&d->next, NULL, memory_order_relaxed);

    struct dpc_queue *dq = &dc->queue;
    dpc_queue_enqueue(dq, d);

    scheduler_force_run_dpcs(global.schedulers[cpu]);

    return true;
}

/* Convenience: enqueue on current cpu */
bool dpc_enqueue_local(struct dpc *d) {
    bool ret = dpc_enqueue_on_cpu(smp_core_id(), d);
    return ret;
}

void dpc_init_percpu(void) {
    global.dpc_data =
        kmalloc(sizeof(struct dpc_cpu) * global.core_count, ALLOC_FLAGS_ZERO);
    size_t i;
    for_each_cpu_id(i) {
        atomic_store_explicit(&global.dpc_data[i].queue.head, NULL,
                              memory_order_relaxed);
    }
}

struct dpc *dpc_init(struct dpc *d, dpc_func_t fn, void *ctx) {
    d->func = fn;
    d->ctx = ctx;
    atomic_store_explicit(&d->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&d->enqueued, false, memory_order_relaxed);
    return d;
}

/* DPC creation helpers */
struct dpc *dpc_create(dpc_func_t fn, void *ctx) {
    struct dpc *d = kmalloc(sizeof(*d));
    if (!d)
        return NULL;

    return dpc_init(d, fn, ctx);
}
