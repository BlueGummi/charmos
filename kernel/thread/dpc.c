#include <acpi/lapic.h>
#include <kassert.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <stdint.h>
#include <thread/dpc.h>

static struct dpc *dpc_steal_queue(struct dpc_queue *dq, struct dpc_cpu *dc) {
    while (true) {
        struct dpc *list =
            atomic_exchange_explicit(&dq->head, NULL, memory_order_acquire);

        if (!list) {
            if (!atomic_load_explicit(&dc->ipi_queued, memory_order_acquire))
                return NULL;

            bool expected = true;
            if (atomic_compare_exchange_strong_explicit(
                    &dc->ipi_queued, &expected, false, memory_order_acq_rel,
                    memory_order_acquire))
                return NULL;

            continue;
        }

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
}

static void dpc_execute_all_in_queue(struct dpc_queue *dq, struct dpc_cpu *dc) {
    while (true) {
        struct dpc *it = dpc_steal_queue(dq, dc);
        if (!it)
            break;

        while (it) {
            atomic_store_explicit(&it->enqueued, false, memory_order_release);
            it->func(it, it->ctx);
            struct dpc *prev = it;
            it = atomic_load_explicit(&it->next, memory_order_relaxed);
            atomic_store_explicit(&prev->executed, true, memory_order_relaxed);
        }
    }
}

void dpc_run_local(void) {
    struct core *me = smp_core();
    if (atomic_exchange(&me->executing_dpcs, true))
        return;

    size_t cpu = me->id;
    enum dpc_event recent = me->dpc_event;
    struct dpc_cpu *dc = &global.dpc_data[cpu];

    dpc_execute_all_in_queue(&dc->queues[recent], dc);

    if (recent != DPC_NONE)
        dpc_execute_all_in_queue(&dc->queues[DPC_NONE], dc);

    /* all clear */
    me->dpc_event = DPC_NONE;
    atomic_store(&me->executing_dpcs, false);
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

bool dpc_enqueue_on_cpu(size_t cpu, struct dpc *d, enum dpc_event e) {
    kassert(d);

    if (atomic_exchange_explicit(&d->enqueued, true, memory_order_acq_rel))
        return false;

    struct dpc_cpu *dc = &global.dpc_data[cpu];

    /* Clear next pointer then push via CAS loop */
    atomic_store_explicit(&d->next, NULL, memory_order_relaxed);

    struct dpc_queue *dq = &dc->queues[e];
    dpc_queue_enqueue(dq, d);

    if (!atomic_exchange_explicit(&dc->ipi_queued, true,
                                  memory_order_acq_rel) &&
        cpu != smp_core_id()) {
        ipi_send(cpu, IRQ_SCHEDULER);
    } else {
        scheduler_mark_self_needs_resched(true);
    }

    return true;
}

/* Convenience: enqueue on current cpu */
bool dpc_enqueue_local(struct dpc *d, enum dpc_event e) {
    bool ret = dpc_enqueue_on_cpu(smp_core_id(), d, e);
    return ret;
}

void dpc_init_percpu(void) {
    global.dpc_data = kzalloc(sizeof(struct dpc_cpu) * global.core_count,
                              ALLOC_PARAMS_DEFAULT);
    size_t i;
    for_each_cpu_id(i) {
        for (size_t j = 0; j < DPC_EVENT_MAX; j++) {
            atomic_store_explicit(&global.dpc_data[i].queues[j].head, NULL,
                                  memory_order_relaxed);
        }
        atomic_store_explicit(&global.dpc_data[i].ipi_queued, 0,
                              memory_order_relaxed);
    }
}

struct dpc *dpc_init(struct dpc *d, dpc_func_t fn, void *ctx) {
    d->func = fn;
    d->ctx = ctx;
    atomic_store_explicit(&d->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&d->enqueued, false, memory_order_relaxed);
    atomic_store_explicit(&d->executed, false, memory_order_relaxed);
    return d;
}

/* DPC creation helpers */
struct dpc *dpc_create(dpc_func_t fn, void *ctx) {
    struct dpc *d = kmalloc(sizeof(*d), ALLOC_PARAMS_DEFAULT);
    if (!d)
        return NULL;

    return dpc_init(d, fn, ctx);
}
