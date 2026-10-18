#include <atomic.h>
#include <irq/ipi.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/tlb.h>
#include <sch/sched.h>
#include <smp/percpu.h>
#include <smp/perdomain.h>
#include <stdint.h>
#include <thread/dpc.h>
#include <time/tsc.h>

#include "tlb_internal.h"

static void tlb_cpu_init(struct tlb_cpu *cpu, cpu_id_t id);
static void tlb_domain_init(struct tlb_domain *cpu, domain_id_t id);

struct tlb_globals tlb_global = {0};
PERDOMAIN_DEFINE(struct tlb_domain, tlb_domains, tlb_domain_init);
PERCPU_DEFINE(struct tlb_cpu, tlb_cpus, tlb_cpu_init);

static void tlb_cpu_init(struct tlb_cpu *cpu, cpu_id_t id) {
    tlb_queue_init(&cpu->eager_ring.queue, TLB_QUEUE_SIZE);
    tlb_queue_init(&cpu->lazy_ring.queue, TLB_QUEUE_SIZE);
    cpu->id = id;
}

static void tlb_domain_init(struct tlb_domain *d, domain_id_t id) {
    d->domain = global.domains[id];
    struct cpu_mask mask = CPU_MASK_INIT;
    domain_set_cpu_mask(&mask, d->domain);

    d->ipi_cost = ipi_send_cost(&mask);
}

static void payload_invalidate(struct tlb_payload *pl) {
    switch (pl->type) {
    case TLB_PAYLOAD_FLUSH: tlb_flush(); break;
    case TLB_PAYLOAD_PAGE: tlb_invlpg(pl->addr); break;
    case TLB_PAYLOAD_RANGE:
        for (vaddr_t start = pl->range.low; start < pl->range.hi;
             start += pl->stride) {
            tlb_invlpg(start);
        }
    }
}

/* Checking flush_req once is ok. If anyone else wants another
 * shootdown, they'd bump and send another IPI anyways */
static void ring_invalidate(struct tlb_ring *ring) {
    uint64_t fr = atomic_load_acq(&ring->flush_req);

    if (fr != atomic_load_rlx(&ring->flush_done)) {
        tlb_queue_drain(&ring->queue);
        tlb_flush();
        atomic_store_release(&ring->flush_done, fr);
    } else {
        struct tlb_queue_entry got;
        while (tlb_queue_dequeue(&ring->queue, &got))
            payload_invalidate(&got.payload);
    }

    atomic_store_release(&ring->done_pos, tlb_queue_consumed(&ring->queue));
}

static void ring_enqueue(struct tlb_ring *ring, struct tlb_payload *pl) {
    struct tlb_queue_entry qent = {.payload = *pl};
    if (!tlb_queue_enqueue(&ring->queue, qent))
        atomic_fetch_add_release(&ring->flush_req, 1);
}

/* Re-load every loop since other shooters can bump the counter */
static void ring_wait(struct tlb_ring *ring) {
    uint64_t want_pos = tlb_queue_produced(&ring->queue);
    uint64_t want_flush = atomic_load_acq(&ring->flush_req);

    while (atomic_load_acq(&ring->done_pos) < want_pos ||
           atomic_load_acq(&ring->flush_done) < want_flush)
        cpu_pause();
}

static void tlb_shootdown_internal(void) {
    ring_invalidate(&PERCPU_PTR(TOPC_IRQ, tlb_cpus)->eager_ring);
}

enum irq_result tlb_shootdown_isr(struct irq_context *rsp, void *ctx) {
    cc_ignore(ctx, rsp);

    tlb_shootdown_internal();
    return IRQ_HANDLED;
}

void tlb_shootdown_full(struct tlb_request rq) {
    if (rq.mode == TLB_MODE_NONE)
        return;

    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return;

    enum irql lirql = irql_raise(IRQL_DISPATCH_LEVEL);

    size_t this_cpu = smp_id(TOPC_IRQL);

    /* TODO: lazy */
    size_t i;
    cpu_mask_for_each(i, rq.targets) {
        if (i == this_cpu) {
            payload_invalidate(&rq.payload);
            continue;
        }

        ring_enqueue(&PERCPU_PTR_FOR(tlb_cpus, i)->eager_ring, &rq.payload);

        ipi_send(i, IRQ_TLB_SHOOTDOWN);
    }

    if (rq.mode == TLB_MODE_SYNC) {
        cpu_mask_for_each(i, rq.targets) {
            if (i == this_cpu)
                continue;

            ring_wait(&PERCPU_PTR_FOR(tlb_cpus, i)->eager_ring);
        }

        atomic_inc_release(&global.pt_epoch);
    }

    irql_lower(lirql);
}

void tlb_init(void) {
    if (tsc_has_invariant()) {
        tlb_global.stamp_source = TLB_STAMP_TSC;
        tlb_global.stamp_guard =
            tsc_global.max_warp + tsc_global.min_rtt + TLB_STAMP_GUARD_SLACK;
    } else {
        tlb_global.stamp_source = TLB_STAMP_GEN;
        tlb_global.stamp_guard = 0;
    }
}
