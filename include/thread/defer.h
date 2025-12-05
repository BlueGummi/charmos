/* @title: Workqueues */
#pragma once

#include <mem/alloc.h>
#include <thread/request.h>
#include <smp/topology.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <structures/list.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>
#include <types/refcount.h>
#include <types/types.h>

typedef void (*work_function)(void *arg, void *arg2);

struct work_args {
    void *arg1;
    void *arg2;
};
#define WORK_ARGS(a, b) ((struct work_args) {.arg1 = a, .arg2 = b})

/* TODO: Merge this with standard workqueue infra */
struct deferred_event {
    size_t timer;
    uint64_t timestamp_ms;
    work_function callback;
    struct work_args args;
    struct deferred_event *next;
};

struct work {
    work_function func;
    struct work_args args;

    struct list_head list_node;

    atomic_bool enqueued;
    atomic_bool active;
    _Atomic uint64_t seq;
};

enum worker_next_action {
    WORKER_NEXT_ACTION_RUN,
    WORKER_NEXT_ACTION_EXIT,
};

struct worker {
    struct thread *thread;       /* Assoc. thread. */
    struct workqueue *workqueue; /* Assoc. wq */

    time_t last_active; /* Monotonic starting time of most
                         * recent work execution */

    time_t inactivity_check_period; /* How much time in between
                                     * timeout GC events */

    time_t start_idle; /* Monotonic starting time of
                        * most recent idle */

    /* Internal flags */
    bool timeout_ran : 1;
    bool should_exit : 1;
    bool is_permanent : 1;
    bool present : 1;
    bool idle : 1;

    enum worker_next_action next_action;

    struct list_head list_node;
};

enum worklist_state {
    WORKLIST_STATE_EMPTY = 0,      /* No works */
    WORKLIST_STATE_READY = 1,      /* Works available */
    WORKLIST_STATE_RUNNING = 2,    /* Executing works */
    WORKLIST_STATE_DESTROYING = 3, /* Destroying */
    WORKLIST_STATE_DEAD = 4,       /* Gone */
};

enum worklist_flags {
    WORKLIST_FLAG_UNBOUND = 1,
};

struct worklist {
    _Atomic enum worklist_state state;
    struct list_head list; /* List of individual works */
    time_t creation_time;
    struct spinlock lock; /* Lock for the list */

    enum worklist_flags flags;
    refcount_t refcount;
};
#define WORKLIST_GET_STATE(wlist) (atomic_load(&wlist->state))
#define WORKLIST_SET_STATE(wlist, state) (atomic_store(&wlist->state, state))

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
    WORKQUEUE_FLAG_PERMANENT = 1 << 1, /* Inverse: On-demand
                                        *
                                        * Permanent workqueues are attached
                                        * to each core and are always Active
                                        * workqueues with on-demand
                                        * worker spawning */

    WORKQUEUE_FLAG_AUTO_SPAWN = 1 << 2, /* Inverse: No auto spawn
                                         *
                                         * This flag allows workqueues
                                         * with multiple workers to
                                         * spawn workers automatically if they
                                         * detect that workers are busy.
                                         *
                                         * Otherwise, that doesn't happen, and
                                         * workers are manually spawned */

    WORKQUEUE_FLAG_UNMIGRATABLE_WORKERS = 1 << 3, /* Inverse: Migratable
                                                   * workers */

    WORKQUEUE_FLAG_NAMED = 1 << 4, /* Has name - will honor (fmt, ...) */

    WORKQUEUE_FLAG_SPAWN_VIA_REQUEST = 1 << 5, /* Spawn threads via
                                                * thread requests instead
                                                * of directly invoking
                                                * the allocator. Will be
                                                * slower, may not always
                                                * be satisfied */

    WORKQUEUE_FLAG_STATIC_WORKERS = 1 << 6, /* `struct worker` will be
                                             * statically allocated
                                             * during workqueue creation.
                                             *
                                             * This allows allocators to
                                             * safely use workqueues that
                                             * dynamically spawn threads,
                                             * but shouldn't be used everywhere
                                             * because it can waste memory */

    WORKQUEUE_FLAG_NO_WORKER_GC = 1 << 7, /* Do not timeout workers */

    WORKQUEUE_FLAG_NO_AUTO_SPAWN = 0,      /* Do not auto spawn workers */
    WORKQUEUE_FLAG_MIGRATABLE_WORKERS = 0, /* Allow migratable workers */
    WORKQUEUE_FLAG_ON_DEMAND = 0, /* Inverse of a permanent workqueue */
    WORKQUEUE_FLAG_NAMELESS = 0,
    WORKQUEUE_FLAG_SPAWN_NORMALLY = 0,
    WORKQUEUE_FLAG_NON_STATIC_WORKERS = 0,
    WORKQUEUE_FLAG_WORKER_GC = 0,

    WORKQUEUE_FLAG_DEFAULTS = WORKQUEUE_FLAG_AUTO_SPAWN | WORKQUEUE_FLAG_NAMED,
};
#define WORKQUEUE_FLAG_SET(q, f) (q->attrs.flags |= f)
#define WORKQUEUE_FLAG_UNSET(q, f) (q->attrs.flags &= ~f)
#define WORKQUEUE_FLAG_TEST(q, f) (q->attrs.flags & f)

enum workqueue_state : uint16_t {
    WORKQUEUE_STATE_DEAD,       /* Gone, about to be freed */
    WORKQUEUE_STATE_DESTROYING, /* Destroying - Do not spawn threads */
    WORKQUEUE_STATE_ACTIVE,     /* Active */
};

#define WORKQUEUE_STATE_SET(q, s) (atomic_store(&q->state, s))
#define WORKQUEUE_STATE_GET(q) (atomic_load(&q->state))

struct workqueue_attributes {
    size_t min_workers; /* If set to 0, this field will be treated as a "1" */
    size_t max_workers;
    size_t capacity;
    time_t spawn_delay;
    nice_t worker_niceness;
    struct {
        uint64_t min;
        uint64_t max;
    } idle_check;

    enum workqueue_flags flags;
    struct cpu_mask worker_cpu_mask;
};

#define WORKQUEUE_DEFAULT_CAPACITY 512
#define WORKQUEUE_DEFAULT_MAX_WORKERS 16
#define WORKQUEUE_DEFAULT_SPAWN_DELAY 150
#define WORKQUEUE_DEFAULT_MIN_IDLE_CHECK SECONDS_TO_MS(2)
#define WORKQUEUE_DEFAULT_MAX_IDLE_CHECK SECONDS_TO_MS(40)

struct workqueue {
    char *name;

    atomic_bool ignore_timeouts;

    struct spinlock work_lock;         /* For works */
    struct spinlock worker_lock;       /* For worker list */
    struct spinlock worker_array_lock; /* For worker array */
    struct spinlock lock;              /* For condvar */

    struct condvar queue_cv;

    struct work *oneshot_works; /* Ringbuffer of ``capacity`` oneshot tasks */
    struct list_head workers;
    struct list_head works;
    struct worker *worker_array; /* if STATIC_WORKER is needed */

    _Atomic uint64_t head;
    _Atomic uint64_t tail;

    atomic_bool spawn_pending;  /* Some enqueue wants us to spawn a worker */
    _Atomic uint32_t num_tasks; /* How many tasks do we have in the ringbuf */

    _Atomic uint32_t num_workers;  /* Current # workers */
    _Atomic uint32_t idle_workers; /* # idle */

    core_t core;
    time_t last_spawn_attempt;

    atomic_flag spawner_flag_internal;

    struct workqueue_attributes attrs;

#ifdef TESTS
    struct workqueue_stats stats;
#endif

    _Atomic enum workqueue_state state; /* Atomic to avoid
                                         * race where stale
                                         * state is seen */
    struct thread_request *request;
    refcount_t refcount;
};

/* Positive values are success with a message,
 * zero is success with nothing special.
 *
 * Negative values are errors */
enum workqueue_error : int32_t {
    WORKQUEUE_ERROR_NEED_NEW_WORKER = 4,  /* For manual worker spawn */
    WORKQUEUE_ERROR_NEED_NEW_WQ = 3,      /* All worker slots filled */
    WORKQUEUE_ERROR_OK = 0,               /* No message */
    WORKQUEUE_ERROR_FULL = -1,            /* Full ringbuffer */
    WORKQUEUE_ERROR_WLIST_EXECUTING = -2, /* Worklist executing */
    WORKQUEUE_ERROR_UNUSABLE = -3,        /* Being destroyed, etc. */
    WORKQUEUE_ERROR_WORK_EXECUTING = -4,
};

void defer_init(void);

/* can only fail from allocation fail */
bool defer_enqueue(work_function func, struct work_args args,
                   uint64_t delay_ms);
void workqueues_permanent_init(void);

struct workqueue *workqueue_create(struct workqueue_attributes *attrs,
                                   const char *fmt, ...);
struct work *work_create(work_function func, struct work_args args);
struct work *work_init(struct work *work, work_function fn,
                       struct work_args args);

void workqueue_free(struct workqueue *queue);
enum workqueue_error workqueue_enqueue_oneshot(struct workqueue *queue,
                                               work_function func,
                                               struct work_args args);

enum workqueue_error workqueue_enqueue(struct workqueue *queue,
                                       struct work *work);

/* Permanent workqueues */
enum workqueue_error workqueue_add_oneshot(work_function func,
                                           struct work_args args);
enum workqueue_error workqueue_add_remote_oneshot(work_function func,
                                                  struct work_args args);
enum workqueue_error workqueue_add_local_oneshot(work_function func,
                                                 struct work_args args);
enum workqueue_error workqueue_add_fast_oneshot(work_function func,
                                                struct work_args args);

enum workqueue_error workqueue_add(struct work *work);
enum workqueue_error workqueue_add_remote(struct work *work);
enum workqueue_error workqueue_add_local(struct work *work);
enum workqueue_error workqueue_add_fast(struct work *work);

void work_execute(struct work *task);
bool workqueue_should_spawn_worker(struct workqueue *queue);
struct worklist *worklist_create(enum worklist_flags);
void worklist_free(struct worklist *wlist);

void workqueue_kick(struct workqueue *queue);
void workqueue_destroy(struct workqueue *queue);

void worker_main(void *);

static inline bool work_active(struct work *work) {
    return atomic_load(&work->active);
}
