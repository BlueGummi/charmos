#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/condvar.h>
#include <sync/spin_lock.h>
#pragma once

/* Must be a power of two for modulo optimization */
#define EVENT_POOL_CAPACITY 512

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
};

struct event_pool {
    struct spinlock lock;
    struct condvar queue_cv;

    struct worker_task tasks[EVENT_POOL_CAPACITY];
    uint64_t head; // producer index
    uint64_t tail; // consumer index

    uint64_t num_tasks;
};

void defer_init(void);

/* can only fail from allocation fail */
bool defer_enqueue(dpc_t func, void *arg, uint64_t delay_ms);
void event_pool_init(uint64_t threads_per_core);

/* these can only fail from allocation fail */
bool event_pool_add(dpc_t func, void *arg);
bool event_pool_add_remote(dpc_t func, void *arg);
bool event_pool_add_local(dpc_t func, void *arg);

static inline void defer_free(void *ptr) {
    event_pool_add_remote(kfree, ptr);
}
