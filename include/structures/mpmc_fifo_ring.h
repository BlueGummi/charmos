/* @title: Multi-Producer Multi-Consumer FIFO Ring */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded Vyukov MPMC FIFO ring, like
 *
 *   MPMC_FIFO_RING_DECLARE(foo_ring, struct foo);
 *
 * emits `struct foo_ring` and foo_ring_{init,destroy,enqueue,dequeue,
 * empty,capacity,drain,produced,consumed} */

struct mpmc_fifo_ring_hdr {
    ca_cacheline_member(size_t, mask); /* capacity - 1, a power of two */
    ca_cacheline_member(atomic_uint64_t, head);
    atomic_uint64_t tail;
};

/* `cap` rounds up to next_pow2 */
void *mpmc_fifo_ring_slots_alloc(struct mpmc_fifo_ring_hdr *h, size_t cap,
                                 size_t stride);
void mpmc_fifo_ring_slots_free(struct mpmc_fifo_ring_hdr *h, void *slots);

static inline cc_always_inline bool
mpmc_fifo_ring_claim(atomic_uint64_t *cursor, size_t mask, void *slots,
                     size_t stride, uint64_t lag, uint64_t *pos_out) {
    while (true) {
        uint64_t pos = atomic_load_rlx(cursor);
        atomic_uint64_t *seq =
            (atomic_uint64_t *) ((uint8_t *) slots + (pos & mask) * stride);
        int64_t diff = (int64_t) atomic_load_acq(seq) - (int64_t) (pos + lag);

        if (diff == 0) {
            if (atomic_cas_weak(cursor, &pos, pos + 1, mo_acq_rel, mo_rlx)) {
                *pos_out = pos;
                return true;
            }
        } else if (diff < 0) {
            return false;
        }
    }
}

static inline cc_always_inline bool
mpmc_fifo_ring_claim_range(struct mpmc_fifo_ring_hdr *h, void *slots,
                           size_t stride, uint64_t *first_out,
                           uint64_t *count_out) {
    while (true) {
        uint64_t t = atomic_load_rlx(&h->tail);
        uint64_t end = atomic_load_acq(&h->head);

        uint64_t p = t;
        while (p != end) {
            atomic_uint64_t *seq = (atomic_uint64_t *) ((uint8_t *) slots +
                                                        (p & h->mask) * stride);
            if (atomic_load_acq(seq) != p + 1)
                break;
            p++;
        }

        if (p == t) {
            if (atomic_load_rlx(&h->tail) != t)
                continue;
            return false;
        }

        uint64_t expected = t;
        if (atomic_cas_weak(&h->tail, &expected, p, mo_acq_rel, mo_rlx)) {
            *first_out = t;
            *count_out = p - t;
            return true;
        }
    }
}

static inline bool
mpmc_fifo_ring_hdr_empty(const struct mpmc_fifo_ring_hdr *h) {
    uint64_t hd = atomic_load_rlx(&h->head);
    uint64_t tl = atomic_load_rlx(&h->tail);
    return hd == tl;
}

#define MPMC_FIFO_RING_DECLARE(name, T)                                        \
    struct name##_slot {                                                       \
        atomic_uint64_t seq;                                                   \
        T val;                                                                 \
    };                                                                         \
                                                                               \
    struct name {                                                              \
        struct name##_slot *slots;                                             \
        struct mpmc_fifo_ring_hdr hdr;                                         \
    };                                                                         \
                                                                               \
    static inline bool name##_init(struct name *q, size_t cap) {               \
        q->slots =                                                             \
            mpmc_fifo_ring_slots_alloc(&q->hdr, cap, sizeof(*q->slots));       \
        return q->slots != NULL;                                               \
    }                                                                          \
                                                                               \
    static inline void name##_destroy(struct name *q) {                        \
        mpmc_fifo_ring_slots_free(&q->hdr, q->slots);                          \
        q->slots = NULL;                                                       \
    }                                                                          \
                                                                               \
    static inline bool name##_enqueue(struct name *q, T v) {                   \
        uint64_t pos;                                                          \
        if (!mpmc_fifo_ring_claim(&q->hdr.head, q->hdr.mask, q->slots,         \
                                  sizeof(*q->slots), 0, &pos))                 \
            return false;                                                      \
                                                                               \
        struct name##_slot *s = &q->slots[pos & q->hdr.mask];                  \
        s->val = v;                                                            \
        atomic_store_release(&s->seq, pos + 1);                                \
        return true;                                                           \
    }                                                                          \
                                                                               \
    static inline bool name##_dequeue(struct name *q, T *out) {                \
        uint64_t pos;                                                          \
        if (!mpmc_fifo_ring_claim(&q->hdr.tail, q->hdr.mask, q->slots,         \
                                  sizeof(*q->slots), 1, &pos))                 \
            return false;                                                      \
                                                                               \
        struct name##_slot *s = &q->slots[pos & q->hdr.mask];                  \
        if (out)                                                               \
            *out = s->val;                                                     \
                                                                               \
        s->val = (T) {0};                                                      \
        atomic_store_release(&s->seq, pos + q->hdr.mask + 1);                  \
        return true;                                                           \
    }                                                                          \
                                                                               \
    /* Drain and return how many were drained */                               \
    static inline size_t name##_drain(struct name *q) {                        \
        uint64_t first, count;                                                 \
        if (!mpmc_fifo_ring_claim_range(&q->hdr, q->slots, sizeof(*q->slots),  \
                                        &first, &count))                       \
            return 0;                                                          \
                                                                               \
        for (uint64_t pos = first; pos != first + count; pos++) {              \
            struct name##_slot *s = &q->slots[pos & q->hdr.mask];              \
            s->val = (T) {0};                                                  \
            atomic_store_release(&s->seq, pos + q->hdr.mask + 1);              \
        }                                                                      \
        return count;                                                          \
    }                                                                          \
                                                                               \
    static inline bool name##_empty(const struct name *q) {                    \
        return mpmc_fifo_ring_hdr_empty(&q->hdr);                              \
    }                                                                          \
                                                                               \
    static inline size_t name##_capacity(const struct name *q) {               \
        return q->hdr.mask + 1;                                                \
    }                                                                          \
                                                                               \
    static inline cc_unused uint64_t name##_produced(struct name *q) {         \
        return atomic_load_acq(&q->hdr.head);                                  \
    }                                                                          \
                                                                               \
    static inline cc_unused uint64_t name##_consumed(struct name *q) {         \
        return atomic_load_acq(&q->hdr.tail);                                  \
    }                                                                          \
                                                                               \
    static_assert(ct_field_offset(struct name##_slot, seq) == 0,               \
                  #name ": seq must lead the slot")
