#pragma once
#include <mem/alloc.h>
#include <misc/list.h>
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

/* TODO: Merge this with standard workqueue infra */
struct deferred_event {
    uint64_t timestamp_ms;
    dpc_t callback;
    void *arg;
    void *arg2;
    struct deferred_event *next;
};

struct work_args {
    void *arg1;
    void *arg2;
};
#define WORK_ARGS(a, b) ((struct work_args) {.arg1 = a, .arg2 = b})

struct work {
    dpc_t func;
    void *arg;
    void *arg2;
    struct list_head list_node;
    atomic_bool cancelled;
    atomic_uint_fast64_t seq;
};

struct worker {
    struct thread *thread;
    time_t last_active;
    time_t inactivity_check_period;
    time_t start_idle;
    bool timeout_ran : 1;
    bool should_exit : 1;
    bool is_permanent : 1;
    bool present : 1;
    bool idle : 1;
};

enum work_list_flags {
    WORK_LIST_FLAG_EXECUTING = 1,
    WORK_LIST_FLAG_ALLOW_MULTIPLE_WORKERS = 1 << 1,
};

struct work_list {
    struct list_head list;          /* List of individual works */
    struct list_head worklist_node; /* Node for list of worklists in a queue */
    time_t creation_time;
    struct spinlock lock;  /* Lock for the list */
    uint64_t work_list_id; /* ID for the list */
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

enum workqueue_flags : uint16_t {
    WORKQUEUE_FLAG_LAZY = 1 << 1, /* Inverse: Active
                                   *
                                   * Lazy workqueues only execute works
                                   * when explicitly 'kicked'.
                                   *
                                   * Active workqueues execute works
                                   * as they are enqueued. The workqueue
                                   * is effectively 'kicked' on each
                                   * enqueue to wake a worker */

    WORKQUEUE_FLAG_PERMANENT = 1 << 2, /* Inverse: On-demand
                                        *
                                        * Permanent workqueues are attached
                                        * to each core and are always Active
                                        * workqueues with on-demand
                                        * worker spawning */

    WORKQUEUE_FLAG_ALLOW_NO_WORKERS = 1 << 3, /* Inverse: Disallow no workers
                                               *
                                               * Allowing no workers allows
                                               * all workers to eventually
                                               * timeout and get cleaned up.
                                               *
                                               * This can sometimes cause
                                               * issues where each
                                               * workqueue must spawn
                                               * the worker manually
                                               * if all workers are gone,
                                               * but can save some memory */

    WORKQUEUE_FLAG_AUTO_SPAWN = 1 << 4, /* Inverse: No auto spawn
                                         *
                                         * This flag allows workqueues
                                         * with multiple workers to
                                         * spawn workers automatically if they
                                         * detect that workers are busy.
                                         *
                                         * Otherwise, that doesn't happen, and
                                         * workers are manually spawned */

    WORKQUEUE_FLAG_UNMIGRATABLE_WORKERS =
        1 << 5, /* Inverse: Migratable workers */

    /* "Inverses of flags" represented as a 0 */
    WORKQUEUE_FLAG_MANUAL_SPAWN = 0,
    WORKQUEUE_FLAG_NOT_LAZY = 0,
    WORKQUEUE_FLAG_MIGRATABLE_WORKERS = 0,
    WORKQUEUE_FLAG_NEEDS_AT_LEAST_ONE_WORKER = 0,
    WORKQUEUE_FLAG_ON_DEMAND = 0,
};
#define WORKQUEUE_FLAG_SET(q, f) (q->attrs.flags |= f)
#define WORKQUEUE_FLAG_UNSET(q, f) (q->attrs.flags &= ~f)
#define WORKQUEUE_FLAG_TEST(q, f) (q->attrs.flags & f)

enum workqueue_state : uint16_t {
    WORKQUEUE_STATE_IDLE,        /* All workers idle */
    WORKQUEUE_STATE_ACTIVE_LOW,  /* 0/4 - 1/4th of workers busy */
    WORKQUEUE_STATE_ACTIVE_MID,  /* 1/4 - 3/4th of workers busy */
    WORKQUEUE_STATE_ACTIVE_HIGH, /* 3/4 - 4/4th of workers busy */
};

#define WORKQUEUE_STATE_SET(q, s) (atomic_store(&q->state, s))
#define WORKQUEUE_STATE_GET(q) (atomic_load(&q->state))

struct workqueue_attributes {
    size_t max_workers;
    size_t capacity;
    time_t spawn_delay;
    struct {
        uint64_t min;
        uint64_t max;
    } inactive_check_period;

    enum workqueue_flags flags;
};

_Static_assert(DEFAULT_MAX_WORKERS < 64, ""); /* Won't fit in our bitmap */
struct workqueue {
    struct spinlock lock; /* Lock for condvar and other things
                           * like the worklists */

    struct condvar queue_cv;

    struct work *tasks;     /* Ringbuffer of ``capacity`` tasks */
    struct worker *workers; /* Array of ``max_workers`` workers */

    atomic_uint_fast64_t head;
    atomic_uint_fast64_t tail;

    atomic_bool spawn_pending; /* Some enqueue wants us to spawn a worker */
    atomic_uint num_tasks;     /* How many tasks do we have in the ringbuf */
    atomic_uint_fast64_t worker_bitmap; /* Bitmap of used/available workers */

    atomic_uint num_workers;  /* Current # workers */
    atomic_uint idle_workers; /* # idle */
    atomic_uint total_spawned;

    time_t last_spawn_attempt;

    atomic_flag spawner_flag_internal;

    struct workqueue_attributes attrs;
    struct list_head worklist_list; /* List of worklists */

#ifdef TESTS
    struct workqueue_stats stats;
#endif

    _Atomic enum workqueue_state state; /* Atomic to avoid
                                         * race where stale
                                         * state is seen */

    refcount_t refcount;
};

/* Positive values are success with a message,
 * zero is success with nothing special.
 *
 * Negative values are errors */
enum workqueue_error : int32_t {
    WORKQUEUE_ERROR_NEED_NEW_WORKER = 4,
    WORKQUEUE_ERROR_NEED_NEW_WQ = 3,
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
bool defer_enqueue(dpc_t func, struct work_args args, uint64_t delay_ms);
void workqueues_permanent_init(void);

struct work *work_create(dpc_t func, struct work_args args);
struct workqueue *workqueue_create(struct workqueue_attributes *attrs);
enum workqueue_error workqueue_add(dpc_t func, struct work_args args);
enum workqueue_error workqueue_add_remote(dpc_t func, struct work_args args);
enum workqueue_error workqueue_add_local(dpc_t func, struct work_args args);
enum workqueue_error workqueue_add_fast(dpc_t func, struct work_args args);

void work_execute(struct work *task);
struct thread *worker_create(void);
struct thread *worker_create_unmigratable();
struct work_list *work_list_create(void);

void worker_main(void);
