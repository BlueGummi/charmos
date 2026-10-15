#include <math/bit.h>
#include <math/min_max.h>
#include <mem/alloc.h>
#include <structures/mpmc_fifo_ring.h>

#define MPMC_FIFO_RING_MIN_CAPACITY ((size_t) 2)

void *mpmc_fifo_ring_slots_alloc(struct mpmc_fifo_ring_hdr *h, size_t cap,
                                 size_t stride) {
    cap = MAX(next_pow2(cap), MPMC_FIFO_RING_MIN_CAPACITY);

    uint8_t *slots = valloc(stride * cap, ALLOC_ZERO);
    if (!slots)
        return NULL;

    for (size_t i = 0; i < cap; i++)
        atomic_store_rlx((atomic_uint64_t *) (slots + i * stride), i);

    h->mask = cap - 1;
    atomic_store_rlx(&h->head, 0);
    atomic_store_rlx(&h->tail, 0);
    return slots;
}

void mpmc_fifo_ring_slots_free(struct mpmc_fifo_ring_hdr *h, void *slots) {
    if (slots)
        vfree(slots);

    h->mask = 0;
}
