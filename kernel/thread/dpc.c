#include <atomic.h>
#include <enforce.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdint.h>
#include <sync/once_latch.h>
#include <thread/dpc.h>

static struct dpc *dpc_steal_queue(struct dpc_queue *dq) {
    struct dpc *list = atomic_xchg_acq(&dq->head, NULL);

    if (!list)
        return NULL;

    struct dpc *rev = NULL;

    while (list) {
        struct dpc *next = atomic_load_rlx(&list->next);
        atomic_store_rlx(&list->next, rev);
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
            struct dpc *next = atomic_load_rlx(&it->next);
            atomic_store_release(&it->enqueued, false);
            it->func(it->a, it->b);
            it = next;
        }
    }
}

void dpc_drain_local(void) {
    struct core *me = smp_core(TOPC_IRQL);
    if (atomic_load_rlx(&me->in_resched))
        return;

    /* Recursion guard */
    if (atomic_exchange(&me->executing_dpcs, true))
        return;

    enforce(irql_get() == IRQL_DISPATCH_LEVEL);

    size_t cpu = me->id;
    struct dpc_cpu *dc = &global.dpc_data[cpu];

again:
    do {
        dpc_execute_all_in_queue(&dc->queue);
    } while (atomic_load_rlx(&dc->queue.head) != NULL);

    atomic_store(&me->executing_dpcs, false);

    /* If an IPI lands here, we need to go do it again,
     * so we don't lose a DPC */
    if (atomic_load(&dc->queue.head) != NULL &&
        !atomic_exchange(&me->executing_dpcs, true))
        goto again;
}

void dpc_run_local(void) {
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    dpc_drain_local();

    /* The raise is only used to satisfy the IRQL requirement here,
     * and the matching lower shouldn't reschedule and recurse,
     * as scheduler_yield() will lower its IRQL too */
    irql_lower_no_resched(irql);
}

void dpc_run_dpcs_from_irq(void) {
    dpc_run_local();
}

static void dpc_queue_enqueue(struct dpc_queue *dq, struct dpc *d) {
    while (true) {
        struct dpc *old_head = atomic_load_acq(&dq->head);
        atomic_store_rlx(&d->next, old_head);
        if (atomic_cas_weak(&dq->head, &old_head, d, mo_release, mo_rlx)) {
            break;
        }
        cpu_pause();
    }
}

bool dpc_enqueue_on_cpu(size_t cpu, struct dpc *d) {
    enforce(d);

    if (atomic_xchg_acq_rel(&d->enqueued, true))
        return false;

    struct dpc_cpu *dc = &global.dpc_data[cpu];

    /* Clear next pointer then push via CAS loop */
    atomic_store_rlx(&d->next, NULL);

    struct dpc_queue *dq = &dc->queue;
    dpc_queue_enqueue(dq, d);

    scheduler_force_run_dpcs(cpu);

    return true;
}

/* Convenience: enqueue on current cpu */
bool dpc_enqueue_local(struct dpc *d) {
    /* Snapshot it */
    bool ret = dpc_enqueue_on_cpu(smp_id_raw(), d);
    return ret;
}

void dpc_init_percpu(void) {
    global.dpc_data = vnews(global.dpc_data, global.core_count, ALLOC_ZERO);
    size_t i;
    for_each_cpu_id(i) {
        atomic_store_rlx(&global.dpc_data[i].queue.head, NULL);
    }
}

struct dpc *dpc_init(struct dpc *d, dpc_func_t fn, void *a, void *b) {
    d->func = fn;
    d->a = a;
    d->b = b;
    atomic_store_rlx(&d->next, NULL);
    atomic_store_rlx(&d->enqueued, false);
    return d;
}

/* DPC creation helpers */
struct dpc *dpc_create(dpc_func_t fn, void *a, void *b) {
    struct dpc *d = vnew(d);
    if (!d)
        return NULL;

    return dpc_init(d, fn, a, b);
}

static void dpc_fanout_wrapper(void *fn, void *latch) {
    any_fn_to(any_fn_from_data(fn), dpc_fanout_fn_t)();
    once_latch_count_down((struct once_latch *) latch);
}

void dpc_fanout(struct dpc *storage, struct cpu_mask cpus, dpc_fanout_fn_t fn) {
    enforce(storage);
    enforce(fn);

    size_t count = cpu_mask_popcount(&cpus);
    if (!count)
        return;

    struct once_latch latch;
    once_latch_init(&latch, count);

    size_t slot = 0;
    size_t cpu;
    cpu_mask_for_each(cpu, cpus) {
        dpc_init(&storage[slot], dpc_fanout_wrapper,
                 any_fn_to_data(any_fn_from(fn)), &latch);
        dpc_enqueue_on_cpu(cpu, &storage[slot]);
        slot++;
    }

    enforce(slot == count);
    once_latch_spin_wait(&latch);
}
