#include <math/bit_ops.h>
#include <math/min_max.h>
#include <mem/alloc.h>
#include <string.h>
#include <structures/spsc_fifo.h>

void spsc_fifo_init_with(struct spsc_fifo *fifo, void *buffer, size_t size) {
    fifo->size = size;
    fifo->mask = size - 1;
    fifo->data = (uint8_t *) buffer;
    atomic_store_explicit(&fifo->head, 0, memory_order_relaxed);
    atomic_store_explicit(&fifo->tail, 0, memory_order_relaxed);
}

bool spsc_fifo_init(struct spsc_fifo *fifo, size_t size) {
    size_t cap = next_pow2(size);
    if (cap < 2) {
        cap = 2;
    }

    uint8_t *buf = kmalloc(cap, ALLOC_FLAGS_ZERO);
    if (!buf) {
        return false;
    }

    spsc_fifo_init_with(fifo, buf, cap);
    return true;
}

void spsc_fifo_destroy(struct spsc_fifo *fifo) {
    if (fifo->data) {
        kfree(fifo->data);
        fifo->data = NULL;
    }
    fifo->size = 0;
    fifo->mask = 0;
}

size_t spsc_fifo_write(struct spsc_fifo *fifo, const void *src, size_t len) {
    size_t head = atomic_load_explicit(&fifo->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&fifo->tail, memory_order_acquire);
    size_t avail = fifo->size - (head - tail);

    len = MIN(len, avail);
    if (len == 0) {
        return 0;
    }

    size_t off = head & fifo->mask;
    size_t l = MIN(len, fifo->size - off);

    memcpy(fifo->data + off, src, l);
    if (len > l) {
        memcpy(fifo->data, (const uint8_t *) src + l, len - l);
    }

    atomic_store_explicit(&fifo->head, head + len, memory_order_release);
    return len;
}

size_t spsc_fifo_read(struct spsc_fifo *fifo, void *dst, size_t len) {
    size_t head = atomic_load_explicit(&fifo->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&fifo->tail, memory_order_relaxed);
    size_t in_use = head - tail;

    len = MIN(len, in_use);
    if (len == 0) {
        return 0;
    }

    size_t off = tail & fifo->mask;
    size_t l = MIN(len, fifo->size - off);

    memcpy(dst, fifo->data + off, l);
    if (len > l) {
        memcpy((uint8_t *) dst + l, fifo->data, len - l);
    }

    atomic_store_explicit(&fifo->tail, tail + len, memory_order_release);
    return len;
}

size_t spsc_fifo_peek(const struct spsc_fifo *fifo, void *dst, size_t len) {
    size_t head = atomic_load_explicit(&fifo->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&fifo->tail, memory_order_relaxed);
    size_t in_use = head - tail;

    len = MIN(len, in_use);
    if (len == 0) {
        return 0;
    }

    size_t off = tail & fifo->mask;
    size_t l = MIN(len, fifo->size - off);

    memcpy(dst, fifo->data + off, l);
    if (len > l) {
        memcpy((uint8_t *) dst + l, fifo->data, len - l);
    }

    return len;
}

bool spsc_fifo_push_ptr(struct spsc_fifo *fifo, const void *ptr) {
    return spsc_fifo_write(fifo, &ptr, sizeof(ptr)) == sizeof(ptr);
}

bool spsc_fifo_pop_ptr(struct spsc_fifo *fifo, void **out_ptr) {
    return spsc_fifo_read(fifo, out_ptr, sizeof(void *)) == sizeof(void *);
}
