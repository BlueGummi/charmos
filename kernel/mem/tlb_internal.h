#include <mem/tlb.h>

#define TLB_STAMP_GUARD_SLACK 1000 /* I just picked a number */
enum tlb_stamp_source {
    TLB_STAMP_GEN, /* One global counter, fallback */
    TLB_STAMP_TSC
};

struct tlb_queue_entry {
    struct tlb_payload payload;
};

MPMC_FIFO_RING_DECLARE(tlb_queue, struct tlb_queue_entry);

struct tlb_ring {
    struct tlb_queue queue;
    tlb_stamp_t stamp_latest;

    atomic_uint64_t done_pos; /* every queue slot < this has been invalidated */
    atomic_uint64_t flush_req;  /* bumped when the queue full */
    atomic_uint64_t flush_done; /* last flush_req covered by a full flush */
};

struct tlb_domain {
    struct domain *domain;
    size_t ipi_cost; /* How many IPIs does it take to
                      * bother everyone in here? */
};

struct tlb_cpu {
    struct tlb_ring eager_ring;
    struct tlb_ring lazy_ring;
    tlb_stamp_t flush_stamp;
    cpu_id_t id;
};

struct tlb_globals {
    enum tlb_stamp_source stamp_source;
    atomic_uint64_t stamp_gen;
    uint64_t stamp_guard; /* Give leeway */
};

extern struct tlb_globals tlb_global;
static inline tlb_stamp_t tlb_stamp_publish(void) {
    if (tlb_global.stamp_source == TLB_STAMP_TSC)
        return rdtsc_after_stores();

    return atomic_fetch_add_acq_rel(&tlb_global.stamp_gen, 1);
}

static inline tlb_stamp_t tlb_stamp_read(void) {
    if (tlb_global.stamp_source == TLB_STAMP_TSC)
        return rdtsc_before_loads();

    return atomic_load_acq(&tlb_global.stamp_gen);
}

/* Is `req` past `seen` ? */
static inline bool tlb_stamp_past(tlb_stamp_t seen, tlb_stamp_t req) {
    return seen > req + tlb_global.stamp_guard;
}
