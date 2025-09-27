#include "string.h"
#include <mem/alloc.h>
#include <sch/daemon.h>

static struct daemon_thread *current_daemon_thread(void) {
    return scheduler_get_curr_thread()->private;
}

static bool mark_daemon_thread_executing(struct daemon_thread *thread,
                                         bool state) {
    return atomic_exchange(&thread->executing_work, state);
}

static size_t total_ts_workers(struct daemon *daemon) {
    return atomic_load(&daemon->attrs.timesharing_threads);
}

static size_t idle_ts_workers(struct daemon *daemon) {
    return atomic_load(&daemon->attrs.idle_timesharing_threads);
}

static size_t max_ts_workers(struct daemon *daemon) {
    return daemon->attrs.max_timesharing_threads;
}

static bool bg_present(struct daemon *daemon) {
    return atomic_load(&daemon->attrs.background_thread_present);
}

static bool set_bg_present(struct daemon *daemon, bool state) {
    return atomic_exchange(&daemon->attrs.background_thread_present, state);
}

static bool ts_busy(struct daemon *d) {
    /* No idle workers */
    return idle_ts_workers(d) == 0 && total_ts_workers(d) > 0;
}

static void daemon_list_add(struct daemon *daemon,
                            struct daemon_thread *thread) {
    enum irql irql = daemon_lock(daemon);
    list_add(&thread->list_node, &daemon->timesharing_threads);
    daemon_unlock(daemon, irql);
}

static void daemon_list_del(struct daemon *daemon,
                            struct daemon_thread *thread) {
    enum irql irql = daemon_lock(daemon);
    list_del(&thread->list_node);
    daemon_unlock(daemon, irql);
}

static void daemon_thread_exit(struct daemon *daemon,
                               struct daemon_thread *self) {
    bool background = self->background;

    if (!background) {
        daemon_list_del(daemon, self);
        atomic_fetch_sub(&daemon->attrs.timesharing_threads, 1);
    } else {
        daemon->background_thread = NULL;
        set_bg_present(daemon, false);
    }

    kfree(self);
    thread_exit();
}

static bool daemon_needs_spawn_worker(struct daemon *d) {
    bool want_spawn = ts_busy(d) && DAEMON_FLAG_TEST(d, DAEMON_FLAG_AUTO_SPAWN);
    bool allowed_to_spawn = total_ts_workers(d) < max_ts_workers(d);
    return want_spawn && allowed_to_spawn;
}

static struct daemon_work *work_on_thread(struct daemon_thread *thread) {
    return thread->background ? thread->daemon->background_work
                              : thread->daemon->timesharing_work;
}

static void daemon_work_execute(struct daemon_work *work,
                                struct daemon_thread *self) {
    self->command = DAEMON_THREAD_COMMAND_DEFAULT;

    mark_daemon_thread_executing(self, true);

    work->function(work, self, work->args.arg1, work->args.arg2);

    mark_daemon_thread_executing(self, false);

    /* Exit if we are destroying */
    if (self->daemon->state == DAEMON_STATE_DESTROYING)
        self->command = DAEMON_THREAD_COMMAND_EXIT;
}

static void daemon_wait(struct daemon *daemon, struct daemon_thread *self) {
    atomic_fetch_add(&daemon->attrs.idle_timesharing_threads, 1);

    if (self->background)
        semaphore_wait(&daemon->bg_sem);
    else
        semaphore_wait(&daemon->ts_sem);

    atomic_fetch_sub(&daemon->attrs.idle_timesharing_threads, 1);

    if (daemon->state == DAEMON_STATE_DESTROYING)
        daemon_thread_exit(daemon, self);
}

void daemon_main(void) {
    struct daemon_thread *self = current_daemon_thread();
    struct daemon *daemon = self->daemon;
    bool timesharing = !self->background;

    if (timesharing)
        atomic_fetch_add(&daemon->attrs.timesharing_threads, 1);

    struct daemon_work *work = work_on_thread(self);

    while (true) {
        daemon_wait(daemon, self);

    start_execute:

        daemon_work_execute(work, self);

        switch (self->command) {
        case DAEMON_THREAD_COMMAND_SLEEP: break; /* Go wait on the semaphore */
        case DAEMON_THREAD_COMMAND_RESTART: goto start_execute;
        case DAEMON_THREAD_COMMAND_EXIT: daemon_thread_exit(daemon, self);
        default:
            k_panic("Unknown daemon thread command with value %u\n",
                    self->command);
        }
    }

    k_panic("Daemon thread should not be able to exit the loop\n");
}

struct daemon_thread *daemon_thread_create(struct daemon *daemon) {
    struct daemon_thread *thread = kzalloc(sizeof(struct daemon_thread));
    if (!thread)
        return NULL;

    thread->daemon = daemon;
    INIT_LIST_HEAD(&thread->list_node);

    struct thread *t = thread_create(daemon_main);
    if (!t) {
        kfree(thread);
        return NULL;
    }

    thread->thread = t;
    t->private = thread;

    return thread;
}

static struct daemon_thread *daemon_thread_create_bg(struct daemon *daemon) {
    struct daemon_thread *ret = daemon_thread_create(daemon);
    if (!ret)
        return NULL;

    ret->background = true;
    thread_set_background(ret->thread);
    return ret;
}

static struct daemon_thread *
daemon_thread_spawn(struct daemon *daemon,
                    struct daemon_thread *(*create)(struct daemon *) ) {
    struct daemon_thread *t = create(daemon);
    if (!t)
        return NULL;

    scheduler_enqueue(t->thread);
    return t;
}

void daemon_thread_destroy_unsafe(struct daemon_thread *dt) {
    thread_free(dt->thread);
    kfree(dt);
}

struct daemon *daemon_create(struct daemon_attributes *attrs,
                             struct daemon_work *timesharing_work,
                             struct daemon_work *background_work,
                             struct workqueue_attributes *wq_attrs,
                             const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    struct daemon *daemon = kzalloc(sizeof(struct daemon));
    struct daemon_thread *dt = NULL, *bg = NULL;

    if (!daemon)
        goto err;

    daemon->attrs = *attrs;

    if (DAEMON_FLAG_TEST(daemon, DAEMON_FLAG_HAS_NAME)) {
        int needed = snprintf(NULL, 0, fmt, args) + 1;
        char *name = kzalloc(needed);
        if (!name)
            goto err;

        snprintf(name, needed, fmt, args);
        daemon->name = name;
    }

    daemon->attrs.background_thread_present = false;
    daemon->attrs.idle_timesharing_threads = 0;
    daemon->attrs.timesharing_threads = 0;

    spinlock_init(&daemon->lock);
    semaphore_init(&daemon->bg_sem, 0);
    semaphore_init(&daemon->ts_sem, 0);

    INIT_LIST_HEAD(&daemon->timesharing_threads);
    daemon->timesharing_work = timesharing_work;
    daemon->background_work = background_work;

    if (DAEMON_FLAG_TEST(daemon, DAEMON_FLAG_HAS_WORKQUEUE)) {
        struct workqueue *wq = workqueue_create(wq_attrs);
        if (!wq)
            goto err;

        daemon->workqueue = wq;
    }

    if (!DAEMON_FLAG_TEST(daemon, DAEMON_FlAG_NO_TS_THREADS) &&
        timesharing_work) {
        dt = daemon_thread_spawn(daemon, daemon_thread_create);
        if (!dt)
            goto err;

        daemon_list_add(daemon, dt);
    }

    if (background_work) {
        bg = daemon_thread_spawn(daemon, daemon_thread_create_bg);
        if (!bg)
            goto err;

        daemon->background_thread = bg;
        set_bg_present(daemon, true);
    }

    daemon->state = DAEMON_STATE_ACTIVE;

    va_end(args);
    return daemon;

err:
    if (dt)
        daemon_thread_destroy_unsafe(dt);

    if (daemon && daemon->name)
        kfree(daemon->name);

    if (daemon)
        kfree(daemon);

    va_end(args);
    return NULL;
}

static void boost_bg_thread_to_ts(struct daemon *daemon) {
    if (daemon->background_thread)
        thread_set_timesharing(daemon->background_thread->thread);
}

/* Assume that all daemons must have daemon works
 * finish executing before they can be safely destroyed */
void daemon_destroy(struct daemon *daemon) {
    /* Make all the threads go sleep on the semaphore */
    daemon->state = DAEMON_STATE_DESTROYING;

    boost_bg_thread_to_ts(daemon);

    /* Wait for all currently running threads to exit or sleep */
    while (total_ts_workers(daemon) > idle_ts_workers(daemon))
        scheduler_yield();

    /* Great, now we send the signal and wake everyone */
    while (total_ts_workers(daemon) > 0) {
        semaphore_post(&daemon->ts_sem);
        scheduler_yield();
    }

    kassert(!total_ts_workers(daemon));
    /* All timesharing threads should be gone now.
     * Time to handle the background thread. */
    while (bg_present(daemon)) {
        semaphore_post(&daemon->bg_sem);
        scheduler_yield();
    }

    daemon->state = DAEMON_STATE_DEAD;

    /* Ok now all threads are gone */
    if (daemon->workqueue) {
        kassert(DAEMON_FLAG_TEST(daemon, DAEMON_FLAG_HAS_WORKQUEUE));
        workqueue_destroy(daemon->workqueue);
    }

    if (daemon->name)
        kfree(daemon->name);

    kfree(daemon);
}

struct daemon_thread *daemon_spawn_worker(struct daemon *daemon) {
    struct daemon_thread *dt =
        daemon_thread_spawn(daemon, daemon_thread_create);

    daemon_list_add(daemon, dt);
    return dt;
}

enum workqueue_error daemon_submit_oneshot_work(struct daemon *daemon,
                                                work_function function,
                                                struct work_args args) {
    kassert(daemon->workqueue &&
            DAEMON_FLAG_TEST(daemon, DAEMON_FLAG_HAS_WORKQUEUE));
    return workqueue_enqueue_oneshot(daemon->workqueue, function, args);
}

enum workqueue_error daemon_submit_work(struct daemon *daemon,
                                        struct work *work) {
    kassert(daemon->workqueue &&
            DAEMON_FLAG_TEST(daemon, DAEMON_FLAG_HAS_WORKQUEUE));
    return workqueue_enqueue(daemon->workqueue, work);
}

void daemon_wake_background_worker(struct daemon *daemon) {
    semaphore_post(&daemon->bg_sem);
}

void daemon_wake_timesharing_worker(struct daemon *daemon) {
    if (daemon_needs_spawn_worker(daemon))
        daemon_spawn_worker(daemon);

    semaphore_post(&daemon->ts_sem);
}

void daemon_print(struct daemon *daemon) {
    struct daemon_attributes *attrs = &daemon->attrs;
    k_printf("struct daemon \"%s\" = {\n",
             daemon->name ? daemon->name : "NULL");
    k_printf("    .attrs = {\n");
    k_printf("                  .max_timesharing_threads = %u\n",
             attrs->max_timesharing_threads);
    k_printf("                  .idle_timesharing_threads = %u\n",
             attrs->idle_timesharing_threads);
    k_printf("                  .timesharing_threads = %u\n",
             attrs->timesharing_threads);
    k_printf("                  .flags = 0b%b\n", attrs->flags);
    k_printf("             }\n");
    k_printf("    .state = %s\n", daemon_state_str(daemon->state));
    k_printf("}\n");
}
