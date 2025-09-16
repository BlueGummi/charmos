#pragma once
#include <misc/list.h>
#include <sch/defer.h>
#include <sch/thread.h>

/* TODO: Signals to signal upwards to exit and such */

/* These commands are sent back up to the daemon
 * thread executing a daemon work and are operated
 * upon the completion of the daemon work */
enum daemon_thread_command {
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
};

struct daemon_work;

typedef void (*daemon_fn)(
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

#define DAEMON_WORK(function, args)                                            \
    (struct daemon_work) {                                                     \
        .function = function, .args = args                                     \
    }

enum daemon_flags {
    DAEMON_FLAG_HAS_WORKQUEUE = 1,
    DAEMON_FLAG_HAS_NAME = 1 << 1,
    DAEMON_FLAG_AUTO_SPAWN = 1 << 2,
    DAEMON_FLAG_NONE = 0,
};

struct daemon_attributes {
    size_t max_timesharing_threads;
    atomic_uint timesharing_threads;
    atomic_uint idle_timesharing_threads;

    enum daemon_flags flags;
};

struct daemon {
    char *name;

    struct list_head timesharing_threads;
    struct daemon_work timesharing_work;

    struct daemon_thread *background_thread;
    struct daemon_work background_work;

    struct workqueue *workqueue;

    struct spinlock lock;

    struct daemon_attributes attrs;
};

#define daemon_thread_from_list_node(ln)                                       \
    container_of(ln, struct daemon_thread, list_node)

struct daemon *daemon_create(struct daemon_attributes *attrs,
                             struct daemon_work timesharing_work,
                             struct daemon_work background_work,
                             struct workqueue_attributes *wq_attrs,
                             const char *fmt, ...);

#define DAEMON_FLAG_TEST(daemon, flag) (daemon->attrs.flags & flag)

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(daemon, lock);
