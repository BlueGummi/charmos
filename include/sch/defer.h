#include <console/printf.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/condvar.h>
#include <sync/spin_lock.h>
#include <types.h>
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

#define MAX_WORKERS 16
struct worker_thread {
    struct thread *thread;
    time_t last_active;
    bool should_exit;
    bool is_permanent;
};

struct event_pool {
    struct spinlock lock;
    struct condvar queue_cv;

    struct worker_task tasks[EVENT_POOL_CAPACITY];
    struct worker_thread threads[MAX_WORKERS];
    uint64_t head; // producer index
    uint64_t tail; // consumer index

    uint64_t num_tasks;

    uint64_t num_workers;
    uint64_t idle_workers;
    uint64_t total_spawned;
    time_t last_spawn_attempt;
};

void defer_init(void);

/* can only fail from allocation fail */
bool defer_enqueue(dpc_t func, void *arg, uint64_t delay_ms);
void event_pool_init();

/* these can only fail from allocation fail */
bool event_pool_add(dpc_t func, void *arg);
bool event_pool_add_remote(dpc_t func, void *arg);
bool event_pool_add_local(dpc_t func, void *arg);
void worker_main(void);

static inline void worker_spawn_on_core(uint64_t core) {
    struct thread *t = thread_spawn_on_core(worker_main, core);
    if (!t)
        k_panic("Failed to spawn worker thread on core %u\n", core);

    t->flags = THREAD_FLAGS_NO_STEAL;
}

static inline void defer_free(void *ptr) {
    event_pool_add_remote(kfree, ptr);
}
