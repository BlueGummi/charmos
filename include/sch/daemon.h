#pragma once
#include <misc/list.h>
#include <sch/defer.h>
#include <sch/thread.h>

/* TODO: Signals to signal upwards to exit and such */

struct daemon_thread {
    struct list_head list_node;
    bool background;
    struct thread *thread;
    struct daemon *daemon;
};

struct daemon_work {
    void (*function)(struct daemon_work *daemon_work, void *arg, void *arg2);
    struct work_args args;
    struct daemon *daemon;
    struct daemon_thread *thread; /* Who is running us? */
    void *private;                /* Whatever anything wants */
};

enum daemon_flags {
    DAEMON_FLAG_HAS_WORKQUEUE = 1,
    DAEMON_FLAG_HAS_NAME = 1 << 1,
};

struct daemon {
    char name[16];

    struct list_head timesharing_threads;
    struct daemon_work timesharing_work;

    struct daemon_thread background_thread;
    struct daemon_work background_work;

    struct workqueue *workqueue;

    struct spinlock lock;

    enum daemon_flags flags;
};

#define daemon_thread_from_list_node(ln)                                       \
    container_of(ln, struct daemon_thread, list_node)

void daemon_init(struct daemon *d, enum daemon_flags flags, const char *fmt, ...);
struct daemon *daemon_create(enum daemon_flags flags, const char *fmt, ...);
