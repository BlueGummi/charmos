/* @title: Multi-Producer Multi-Consumer Queue */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mpmc_slot {
    _Atomic uint64_t seq;
    uintptr_t data;
};

struct mpmc_queue {
    size_t capacity;
    size_t mask;
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    struct mpmc_slot *slots;
};

#define MPMC_QUEUE_INIT                                                        \
    (struct mpmc_queue) {                                                      \
        .capacity = 0, .mask = 0, .head = 0, .tail = 0, .slots = NULL          \
    }

bool mpmc_queue_init(struct mpmc_queue *q, size_t capacity);

void mpmc_queue_init_static(struct mpmc_queue *q, struct mpmc_slot *slots,
                            size_t capacity);

void mpmc_queue_destroy(struct mpmc_queue *q);

bool mpmc_queue_enqueue(struct mpmc_queue *q, void *ptr);
bool mpmc_queue_enqueue_uintptr(struct mpmc_queue *q, uintptr_t val);

bool mpmc_queue_dequeue(struct mpmc_queue *q, void **out_ptr);
bool mpmc_queue_dequeue_uintptr(struct mpmc_queue *q, uintptr_t *out_val);

static inline bool mpmc_queue_empty(const struct mpmc_queue *q) {
    uint64_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint64_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    return h == t;
}

static inline size_t mpmc_queue_capacity(const struct mpmc_queue *q) {
    return q->capacity;
}
