#include <math/bit_ops.h>
#include <mem/alloc.h>
#include <structures/mpmc_queue.h>

void mpmc_queue_init_static(struct mpmc_queue *q, struct mpmc_slot *slots,
                            size_t capacity) {
    q->capacity = capacity;
    q->mask = capacity - 1;
    q->slots = slots;

    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);

    for (size_t i = 0; i < capacity; i++) {
        atomic_store_explicit(&q->slots[i].seq, i, memory_order_relaxed);
        q->slots[i].data = 0;
    }
}

bool mpmc_queue_init(struct mpmc_queue *q, size_t capacity) {
    size_t cap = next_pow2(capacity);
    if (cap < 2) {
        cap = 2;
    }

    struct mpmc_slot *slots =
        kmalloc(sizeof(struct mpmc_slot) * cap, ALLOC_FLAGS_ZERO);
    if (!slots) {
        return false;
    }

    mpmc_queue_init_static(q, slots, cap);
    return true;
}

void mpmc_queue_destroy(struct mpmc_queue *q) {
    if (q->slots) {
        kfree(q->slots);
        q->slots = NULL;
    }
    q->capacity = 0;
    q->mask = 0;
}

bool mpmc_queue_enqueue_uintptr(struct mpmc_queue *q, uintptr_t val) {
    uint64_t pos;
    struct mpmc_slot *slot;
    uint64_t seq;
    int64_t diff;

    while (true) {
        pos = atomic_load_explicit(&q->head, memory_order_relaxed);
        slot = &q->slots[pos & q->mask];
        seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        diff = (int64_t) seq - (int64_t) pos;

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->head, &pos, pos + 1,
                                                      memory_order_acq_rel,
                                                      memory_order_relaxed)) {
                slot->data = val;
                atomic_store_explicit(&slot->seq, pos + 1,
                                      memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false; /* Queue is full */
        }
    }
}

bool mpmc_queue_enqueue(struct mpmc_queue *q, void *ptr) {
    return mpmc_queue_enqueue_uintptr(q, (uintptr_t) ptr);
}

bool mpmc_queue_dequeue_uintptr(struct mpmc_queue *q, uintptr_t *out_val) {
    uint64_t pos;
    struct mpmc_slot *slot;
    uint64_t seq;
    int64_t diff;

    while (true) {
        pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
        slot = &q->slots[pos & q->mask];
        seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        diff = (int64_t) seq - (int64_t) (pos + 1);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->tail, &pos, pos + 1,
                                                      memory_order_acq_rel,
                                                      memory_order_relaxed)) {
                if (out_val) {
                    *out_val = slot->data;
                }
                slot->data = 0;
                atomic_store_explicit(&slot->seq, pos + q->capacity,
                                      memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false;
        }
    }
}

bool mpmc_queue_dequeue(struct mpmc_queue *q, void **out_ptr) {
    uintptr_t val = 0;
    if (mpmc_queue_dequeue_uintptr(q, &val)) {
        if (out_ptr) {
            *out_ptr = (void *) val;
        }
        return true;
    }
    return false;
}
