/* @title: TLB */
#pragma once
#include <atomic.h>
#include <compiler/diagnostic.h>
#include <math/range.h>
#include <stdint.h>
#include <structures/cpu_mask.h>
#include <structures/mpmc_fifo_ring.h>
#include <types/types.h>

/* per-cpu */
#define TLB_QUEUE_SIZE 64

/*
 * The way our TLB shootdowns work is that we have a two tiered queue:
 *
 * (1) Eager
 * (2) Lazy
 *
 * We group by domains, which are NUMA nodes when available, or grouped by
 * LLC/arbitrary when on UMA. On x86 (currently the only supported architecture),
 * we use x2APIC cluster IPI batching to reduce IPI costs, and precompute
 * the IPI costs for a given domain to determine whether a domain IPI
 * should be chosen, or individual per-CPU IPIs.
 *
 * An issue with lazy TLB invalidations, however, is that thread migrations can
 * affect coherency. For instance, if thread A is running on CPU 0, and it
 * invalidates all relevant lazy queues, and uses that premise to then
 * read some virtual memory, if it gets migrated to CPU 1, the
 * TLB could end up stale.
 *
 * To mitigate this, we introduce an idea of "timestamping" TLB invalidations.
 * This is done via the TSC on x86, and the idea is that upon an invalidation
 * of the lazy queue, you record a timestamp for when that happened, on the
 * current thread.
 *
 */

typedef uint64_t tlb_stamp_t;

/* A type describes what an actual operation is */
enum tlb_payload_type {
    TLB_PAYLOAD_PAGE,  /* Invalidate just this one page */
    TLB_PAYLOAD_RANGE, /* Invalidate this range */
    TLB_PAYLOAD_FLUSH  /* Completely flush everything */
};

/* Modes specify how to deliver. SYNC and ASYNC are eager */
enum tlb_mode {
    TLB_MODE_SYNC,  /* Send the IPI out, wait for responses */
    TLB_MODE_ASYNC, /* Fire and forget */
    TLB_MODE_LAZY,  /* Tell the other CPUs about it, but no enqueues */
    TLB_MODE_NONE   /* Local only: no remote CPUs are told (caller batches) */
};

struct tlb_payload {
    enum tlb_payload_type type;
    union {
        vaddr_t addr;
        struct {
            RANGE_DEFINE(vaddr_t, range);
            size_t stride;
        };
    };
};

struct tlb_request {
    enum tlb_mode mode;
    struct cpu_mask targets;
    struct tlb_payload payload;
};

void tlb_init(void);
enum irq_result tlb_shootdown_isr(struct irq_context *rsp, void *ctx);
void tlb_shootdown_full(struct tlb_request rq);
void tlb_invalidate_lazy(void);

#define tlb_shootdown(...)                                                     \
    tlb_shootdown_full(cc_wno_override_init_expr(                              \
        struct tlb_request,                                                    \
        ((struct tlb_request) {.mode = TLB_MODE_SYNC,                          \
                               .targets = CPU_MASK_INIT_ALL,                   \
                               .payload.type = TLB_PAYLOAD_PAGE,               \
                               ##__VA_ARGS__})))
