#include <acpi/lapic.h>
#include <kassert.h>
#include <thread/dpc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <stdint.h>

void dpc_run_local(void) {

    size_t cpu = smp_core_id();
    struct dpc_cpu *dc = &global.dpc_data[cpu];

    for (;;) {
        /* steal list (atomic) */
        struct dpc *list =
            atomic_exchange_explicit(&dc->head, NULL, memory_order_acquire);

        if (!list) {
            if (atomic_load_explicit(&dc->dpc_queued, memory_order_acquire) ==
                0)
                return;

            uint8_t expected = 1;
            if (atomic_compare_exchange_strong_explicit(
                    &dc->dpc_queued, &expected, 0, memory_order_acq_rel,
                    memory_order_acquire)) {
                return;
            }

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

        struct dpc *it = rev;
        while (it) {
            atomic_store_explicit(&it->enqueued, false, memory_order_release);
            it->func(it->ctx);
            it = atomic_load_explicit(&it->next, memory_order_relaxed);
        }

        /* loop to steal any newly enqueued DPCs */
    }
}

bool dpc_enqueue_on_cpu(size_t cpu, struct dpc *d) {
    kassert(cpu < global.core_count);
    kassert(d != NULL);
    /* Prevent double-enqueue */
    bool already =
        atomic_exchange_explicit(&d->enqueued, true, memory_order_acq_rel);
    if (already)
        return false;

    struct dpc_cpu *dc = &global.dpc_data[cpu];

    /* Clear next pointer then push via CAS loop */
    atomic_store_explicit(&d->next, NULL, memory_order_relaxed);

    for (;;) {
        struct dpc *old_head =
            atomic_load_explicit(&dc->head, memory_order_acquire);
        atomic_store_explicit(&d->next, old_head, memory_order_relaxed);
        if (atomic_compare_exchange_weak_explicit(&dc->head, &old_head, d,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            break;
        }
        cpu_relax();
    }

    uint8_t old =
        atomic_exchange_explicit(&dc->dpc_queued, 1, memory_order_acq_rel);
    if (old == 0 && cpu != smp_core_id()) {
        ipi_send(cpu, IRQ_SCHEDULER);
    }

    return true;
}

/* Convenience: enqueue on current cpu */
bool dpc_enqueue_local(struct dpc *d) {
    bool ret = dpc_enqueue_on_cpu(smp_core_id(), d);
    return ret;
}

/* Helpers: init */
void dpc_init_percpu(void) {
    global.dpc_data = kzalloc(sizeof(struct dpc_cpu) * global.core_count,
                              ALLOC_PARAMS_DEFAULT);
    size_t i;
    for_each_cpu_id(i) {
        atomic_store_explicit(&global.dpc_data[i].head, NULL,
                              memory_order_relaxed);
        atomic_store_explicit(&global.dpc_data[i].dpc_queued, 0,
                              memory_order_relaxed);
    }
}

/* DPC creation helpers */
struct dpc *dpc_create(dpc_func_t fn, void *ctx) {
    struct dpc *d = kmalloc(sizeof(*d), ALLOC_PARAMS_DEFAULT);
    if (!d)
        return NULL;

    d->func = fn;
    d->ctx = ctx;
    atomic_store_explicit(&d->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&d->enqueued, false, memory_order_relaxed);
    return d;
}
