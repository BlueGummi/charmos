#include <math/bit.h>
#include <math/min_max.h>
#include <mem/alloc.h>
#include <string.h>
#include <structures/spsc_fifo.h>

#define SPSC_FIFO_MIN_CAPACITY ((size_t) 2)

void spsc_fifo_init_with(struct spsc_fifo *fifo, void *buffer, size_t size) {
    fifo->size = size;
    fifo->mask = size - 1;
    fifo->data = (uint8_t *) buffer;
    atomic_store_relaxed(&fifo->head, 0);
    atomic_store_relaxed(&fifo->tail, 0);
}

bool spsc_fifo_init(struct spsc_fifo *fifo, size_t size) {
    size_t cap = MAX(next_pow2(size), SPSC_FIFO_MIN_CAPACITY);

    uint8_t *buf = valloc(cap, ALLOC_ZERO);
    if (!buf) {
        return false;
    }

    spsc_fifo_init_with(fifo, buf, cap);
    return true;
}

void spsc_fifo_destroy(struct spsc_fifo *fifo) {
    if (fifo->data) {
        vfree(fifo->data);
        fifo->data = NULL;
    }
    fifo->size = 0;
    fifo->mask = 0;
}

size_t spsc_fifo_write(struct spsc_fifo *fifo, const void *src, size_t len) {
    size_t head = atomic_load_relaxed(&fifo->head);
    size_t tail = atomic_load_acq(&fifo->tail);
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

    atomic_store_release(&fifo->head, head + len);
    return len;
}

size_t spsc_fifo_read(struct spsc_fifo *fifo, void *dst, size_t len) {
    size_t head = atomic_load_acq(&fifo->head);
    size_t tail = atomic_load_relaxed(&fifo->tail);
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

    atomic_store_release(&fifo->tail, tail + len);
    return len;
}

size_t spsc_fifo_peek(const struct spsc_fifo *fifo, void *dst, size_t len) {
    size_t head = atomic_load_acq(&fifo->head);
    size_t tail = atomic_load_relaxed(&fifo->tail);
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
