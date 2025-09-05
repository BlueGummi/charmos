#pragma once
#include <mem/alloc.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>
#include <types/refcount.h>
#include <types/types.h>

/* TODO: Make the values below scale */

/* Must be a power of two for modulo optimization */
#define DEFAULT_WORKQUEUE_CAPACITY 512
#define DEFAULT_MAX_WORKERS 16
#define DEFAULT_SPAWN_DELAY 150 /* 150ms delay between worker thread spawns */
#define DEFAULT_MIN_INTERACTIVITY_CHECK_PERIOD SECONDS_TO_MS(2)
#define DEFAULT_MAX_INTERACTIVITY_CHECK_PERIOD SECONDS_TO_MS(10)

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

struct slot {
    atomic_uint_fast64_t seq;
    struct worker_task task;
};

struct worker_thread {
    struct thread *thread;
    time_t last_active;
    time_t inactivity_check_period;
    bool timeout_ran;
    bool should_exit;
    bool is_permanent;
    bool present;
    bool idle;
    time_t start_idle;
};

/* TODO: Get in profiling.h and put these under there */
#ifdef TESTS
struct workqueue_stats {
    uint64_t total_tasks_added;    /* Total # of tasks submitted to the queue */
    uint64_t total_tasks_executed; /* Number of tasks successfully executed */
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

_Static_assert(DEFAULT_MAX_WORKERS < 64, ""); /* Won't fit in our bitmap */
struct workqueue {
    struct spinlock lock;
    struct condvar queue_cv;

    struct slot *tasks;            /* Ringbuffer of ``capacity`` tasks */
    struct worker_thread *workers; /* Array of ``max_workers`` workers */
    size_t max_workers;
    size_t capacity;

    atomic_uint_fast64_t head;
    atomic_uint_fast64_t tail;

    atomic_bool spawn_pending; /* Some enqueue wants us to spawn a worker */
    atomic_uint num_tasks;     /* How many tasks do we have in the ringbuf */
    atomic_uint_fast64_t worker_bitmap; /* Bitmap of used/available workers */

    atomic_uint num_workers; /* Current # workers */
    atomic_uint idle_workers; /* # idle */
    atomic_uint total_spawned;

    time_t spawn_delay;
    time_t last_spawn_attempt;

    struct {
        uint64_t min;
        uint64_t max;
    } interactivity_check_period;

    uint64_t core;

    atomic_flag spawner_flag;
#ifdef TESTS
    struct workqueue_stats stats;
#endif

    refcount_t refcount;
};

/* Positive values are success with a message,
 * zero is success with nothing special.
 *
 * Negative values are errors */
enum workqueue_error : int32_t {
    WORKQUEUE_ERROR_NEED_NEW_THREAD = 4,
    WORKQUEUE_ERROR_OK = 0,
    WORKQUEUE_ERROR_FULL = -1,
};

static inline bool workqueue_get(struct workqueue *queue) {
    return refcount_inc(&queue->refcount);
}

static inline void workqueue_put(struct workqueue *queue) {
    if (refcount_dec_and_test(&queue->refcount))
        return; /* TODO: free */
}

void defer_init(void);

/* can only fail from allocation fail */
bool defer_enqueue(dpc_t func, void *arg, void *arg2, uint64_t delay_ms);
void workqueue_init();

bool workqueue_add(dpc_t func, void *arg, void *arg2);
bool workqueue_add_remote(dpc_t func, void *arg, void *arg2);
bool workqueue_add_local(dpc_t func, void *arg, void *arg2);
bool workqueue_add_fast(dpc_t func, void *arg, void *arg2);

void worker_main(void);
