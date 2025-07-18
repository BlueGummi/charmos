#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/condvar.h>
#include <sync/spin_lock.h>
#pragma once

typedef void (*dpc_t)(void *arg);

struct deferred_event {
    uint64_t timestamp_ms;
    dpc_t callback;
    void *arg;
    struct deferred_event *next;
};

struct worker_task {
    dpc_t func;
    void *arg;
    struct worker_task *next;
};

struct event_pool {
    struct worker_task *head;
    struct worker_task *tail;
    struct spinlock lock;
    struct condvar queue_cv;
};

void defer_init(void);

/* can only fail from allocation fail */
bool defer_enqueue(dpc_t func, void *arg, uint64_t delay_ms);

void event_pool_init(uint64_t num_threads);

/* can only fail from allocation fail */
bool event_pool_add(dpc_t func, void *arg);

static inline void defer_free(void *ptr) {
    event_pool_add(kfree, ptr);
}
