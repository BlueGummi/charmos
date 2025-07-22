#include <console/printf.h>
#include <mem/alloc.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/condvar.h>
#include <sync/spin_lock.h>
#include <types.h>
#pragma once

/* Must be a power of two for modulo optimization */
#define EVENT_POOL_CAPACITY 512
#define MAX_WORKERS 16
#define SPAWN_DELAY 25 /* 25ms delay between worker thread spawns */
#define MIN_INTERACTIVITY_CHECK_PERIOD SECONDS_TO_MS(2)
#define MAX_INTERACTIVITY_CHECK_PERIOD SECONDS_TO_MS(10)

typedef void (*dpc_t)(void *arg, void *arg2);

struct deferred_event {
    uint64_t timestamp_ms;
    dpc_t callback;
    void *arg;
    void *arg2;
    struct deferred_event *next;
};

struct worker_task {
    dpc_t func;
    void *arg;
    void *arg2;
};

struct worker_thread {
    struct thread *thread;
    time_t last_active;
    time_t inactivity_check_period;
    bool timeout_ran;
    bool should_exit;
    bool is_permanent;
    bool present;
};

#ifdef TESTS
struct event_pool_stats {
    uint64_t total_tasks_added;     /* Total # of tasks submitted to the pool */
    uint64_t total_tasks_executed;  /* Number of tasks successfully executed */
    uint64_t total_workers_spawned; /* Total worker threads spawned */
    uint64_t total_worker_exits;    /* Total workers that exited */
    uint64_t max_queue_length;      /* Max observed length of the task queue */
    uint64_t current_queue_length;  /* Current length of the queue */
    uint64_t total_spawn_attempts;  /* # times spawn_worker was attempted */
    uint64_t total_spawn_failures;  /* Number of times spawn_worker failed */
    uint64_t num_idle_workers;      /* Snapshot of current idle workers */
    uint64_t num_active_workers;    /* Snapshot of current active workers */
};
#endif

struct event_pool {
    struct spinlock lock;
    struct condvar queue_cv;

    struct worker_task tasks[EVENT_POOL_CAPACITY];
    struct worker_thread threads[MAX_WORKERS];
    uint64_t head; // producer index
    uint64_t tail; // consumer index

    atomic_uint num_tasks;

    atomic_uint num_workers;
    atomic_uint idle_workers;
    atomic_uint total_spawned;

    time_t last_spawn_attempt;
    uint64_t core;

    bool currently_spawning;
#ifdef TESTS
    struct event_pool_stats stats;
#endif
};

void defer_init(void);

/* can only fail from allocation fail */
bool defer_enqueue(dpc_t func, void *arg, void *arg2, uint64_t delay_ms);
void event_pool_init();

/* these can only fail from allocation fail */
bool event_pool_add(dpc_t func, void *arg, void *arg2);
bool event_pool_add_remote(dpc_t func, void *arg, void *arg2);
bool event_pool_add_local(dpc_t func, void *arg, void *arg2);
void worker_main(void);

static void kfree_deferrable(void *ptr, void *) {
    kfree(ptr);
}

static inline void defer_free(void *ptr) {
    event_pool_add_remote(kfree_deferrable, ptr, NULL);
}
