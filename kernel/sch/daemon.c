#include "string.h"
#include <mem/alloc.h>
#include <sch/daemon.h>

void daemon_main(void) {}

struct daemon_thread *daemon_thread_create(struct daemon *daemon,
                                           bool background) {
    struct daemon_thread *thread = kmalloc(sizeof(struct daemon_thread));
    if (!thread)
        return NULL;

    thread->background = background;
    thread->daemon = daemon;
    INIT_LIST_HEAD(&thread->list_node);

    struct thread *t = thread_create(daemon_main);
    if (!t) {
        kfree(thread);
        return NULL;
    }

    if (background)
        thread_set_background(t);

    t->private = thread;

    return thread;
}

void daemon_thread_destroy(struct daemon_thread *dt) {
    thread_free(dt->thread);
    kfree(dt);
}

struct daemon *daemon_create(struct daemon_attributes *attrs,
                             struct daemon_work timesharing_work,
                             struct daemon_work background_work,
                             struct workqueue_attributes *wq_attrs,
                             const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    int needed = snprintf(NULL, 0, fmt, args) + 1;

    struct daemon *daemon = kzalloc(sizeof(struct daemon));
    if (!daemon)
        goto err;

    daemon->attrs = *attrs;

    if (DAEMON_FLAG_TEST(daemon, DAEMON_FLAG_HAS_NAME)) {
        char *name = kmalloc(needed);
        if (!name)
            goto err;

        snprintf(name, needed, fmt, args);
        daemon->name = name;
    }

    daemon->attrs.idle_timesharing_threads = 0;
    daemon->attrs.timesharing_threads = 0;

    spinlock_init(&daemon->lock);
    INIT_LIST_HEAD(&daemon->timesharing_threads);
    daemon->timesharing_work = timesharing_work;
    daemon->background_work = background_work;

    if (DAEMON_FLAG_TEST(daemon, DAEMON_FLAG_HAS_WORKQUEUE)) {
        struct workqueue *wq = workqueue_create(wq_attrs);
        if (!wq)
            goto err;

        daemon->workqueue = wq;
    }

    struct daemon_thread *dt = daemon_thread_create(daemon, false);
    if (!dt)
        goto err;

    struct daemon_thread *dtb = daemon_thread_create(daemon, true);
    if (!dt) {
        daemon_thread_destroy(dt);
        goto err;
    }

    list_add(&dt->list_node, &daemon->timesharing_threads);

    daemon->background_thread = dtb;

    va_end(args);
    return daemon;

err:
    if (daemon && daemon->name)
        kfree(daemon->name);

    if (daemon)
        kfree(daemon);

    va_end(args);
    return NULL;
}
