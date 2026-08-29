/* @title: Single-Producer Single-Consumer Lock-Free FIFO */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct spsc_fifo {
    _Atomic size_t head; /* Written by Producer */
    _Atomic size_t tail; /* Written by Consumer */
    size_t size;         /* Capacity (pow2) */
    size_t mask;         /* size - 1 */
    uint8_t *data;
};

#define SPSC_FIFO_INIT                                                         \
    (struct spsc_fifo) {                                                       \
        .head = 0, .tail = 0, .size = 0, .mask = 0, .data = NULL               \
    }

/* (rounded to power of 2) */
bool spsc_fifo_init(struct spsc_fifo *fifo, size_t size);

void spsc_fifo_init_with(struct spsc_fifo *fifo, void *buffer, size_t size);

void spsc_fifo_destroy(struct spsc_fifo *fifo);

size_t spsc_fifo_write(struct spsc_fifo *fifo, const void *src, size_t len);

size_t spsc_fifo_read(struct spsc_fifo *fifo, void *dst, size_t len);

/* peek up to `len` bytes into `dst` without consuming */
size_t spsc_fifo_peek(const struct spsc_fifo *fifo, void *dst, size_t len);

bool spsc_fifo_push_ptr(struct spsc_fifo *fifo, const void *ptr);
bool spsc_fifo_pop_ptr(struct spsc_fifo *fifo, void **out_ptr);

static inline size_t spsc_fifo_len(const struct spsc_fifo *fifo) {
    size_t h = atomic_load_explicit(&fifo->head, memory_order_acquire);
    size_t t = atomic_load_explicit(&fifo->tail, memory_order_relaxed);
    return h - t;
}

static inline size_t spsc_fifo_avail(const struct spsc_fifo *fifo) {
    return fifo->size - spsc_fifo_len(fifo);
}

static inline bool spsc_fifo_is_empty(const struct spsc_fifo *fifo) {
    return spsc_fifo_len(fifo) == 0;
}

static inline bool spsc_fifo_is_full(const struct spsc_fifo *fifo) {
    return spsc_fifo_avail(fifo) == 0;
}

static inline void spsc_fifo_reset(struct spsc_fifo *fifo) {
    atomic_store_explicit(&fifo->head, 0, memory_order_relaxed);
    atomic_store_explicit(&fifo->tail, 0, memory_order_relaxed);
}
