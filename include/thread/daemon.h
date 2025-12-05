/* @title: Daemons */
#pragma once
#include <structures/list.h>
#include <thread/defer.h>
#include <thread/thread.h>
#include <sync/semaphore.h>

/* These commands are sent back up to the daemon
 * thread executing a daemon work and are operated
 * upon the completion of the daemon work */
enum daemon_thread_command {
    DAEMON_THREAD_COMMAND_NONE,
    DAEMON_THREAD_COMMAND_EXIT,    /* Daemon thread exit */
    DAEMON_THREAD_COMMAND_RESTART, /* Restart the function */
    DAEMON_THREAD_COMMAND_SLEEP,   /* Go to sleep */
    DAEMON_THREAD_COMMAND_DEFAULT =
        DAEMON_THREAD_COMMAND_SLEEP, /* By default we sleep after
                                      * the daemon work returns */
};

struct daemon_thread {
    struct list_head list_node;
    bool background;
    struct thread *thread;
    struct daemon *daemon; /* What daemon are we attached to? */

    atomic_bool executing_work;
    enum daemon_thread_command command;
};

struct daemon_work;

typedef enum daemon_thread_command (*daemon_fn)(
    struct daemon_work *work,       /* Work in question */
    struct daemon_thread *executor, /* Current executing thread */
    void *arg,                      /* Daemon work provided arg1 */
    void *arg2                      /* Daemon work provided arg2 */
);

struct daemon_work {
    daemon_fn function;
    struct work_args args;
    struct daemon *daemon;
    void *private; /* Whatever anything wants */
};
#define DAEMON_WORK_FROM(fn, a)                                                \
    ((struct daemon_work) {.function = fn, .args = a})

enum daemon_flags {
    DAEMON_FLAG_HAS_WORKQUEUE = 1,
    DAEMON_FLAG_HAS_NAME = 1 << 1,
    DAEMON_FLAG_AUTO_SPAWN = 1 << 2,
    DAEMON_FLAG_NO_TS_THREADS = 1 << 3,
    DAEMON_FLAG_UNMIGRATABLE_THREADS = 1 << 4,
    DAEMON_FLAG_NONE = 0,
};

struct daemon_attributes {
    size_t max_timesharing_threads;
    atomic_size_t timesharing_threads;
    atomic_size_t idle_timesharing_threads;
    atomic_bool background_thread_present;

    /* TODO: Once we get threads to have CPUmasks
     * that indicate where they can be run, I'll
     * need to fix this and just allow assigned CPUmasks */
    int64_t thread_cpu;

    enum daemon_flags flags;
};

enum daemon_state {
    DAEMON_STATE_ACTIVE,
    DAEMON_STATE_DESTROYING,
    DAEMON_STATE_DEAD,
};

static inline const char *daemon_state_str(enum daemon_state s) {
    switch (s) {
    case DAEMON_STATE_ACTIVE: return "ACTIVE";
    case DAEMON_STATE_DESTROYING: return "DESTROYING";
    case DAEMON_STATE_DEAD: return "DEAD";
    }
    return "UNKNOWN";
}

struct daemon {
    char *name;
    struct semaphore ts_sem;
    struct semaphore bg_sem;

    struct list_head timesharing_threads;
    struct daemon_work *timesharing_work;

    struct daemon_thread *background_thread;
    struct daemon_work *background_work;

    struct workqueue *workqueue;

    struct daemon_attributes attrs;

    struct spinlock lock;
    enum daemon_state state;
};

#define daemon_thread_from_list_node(ln)                                       \
    container_of(ln, struct daemon_thread, list_node)

struct daemon *daemon_create(struct daemon_attributes *attrs,
                             struct daemon_work *timesharing_work,
                             struct daemon_work *background_work,
                             struct workqueue_attributes *wq_attrs,
                             const char *fmt, ...);

void daemon_destroy(struct daemon *daemon);

struct daemon_thread *daemon_spawn_worker(struct daemon *daemon);

enum workqueue_error daemon_submit_oneshot_work(struct daemon *daemon,
                                                work_function func,
                                                struct work_args args);

enum workqueue_error daemon_submit_work(struct daemon *daemon,
                                        struct work *work);

void daemon_print(struct daemon *daemon);
void daemon_wake_background_worker(struct daemon *daemon);
void daemon_wake_timesharing_worker(struct daemon *daemon);

#define DAEMON_FLAG_TEST(daemon, flag) (daemon->attrs.flags & flag)

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(daemon, lock);
