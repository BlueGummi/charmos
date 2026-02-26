/* @title: Real-time scheduling */
#pragma once
#include <log.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <thread/thread_types.h>

LOG_SITE_EXTERN(rt_sched);
LOG_HANDLE_EXTERN(rt_sched);

#define rt_sched_log(lvl, fmt, ...)                                            \
    log(LOG_SITE(rt_sched), LOG_HANDLE(rt_sched), lvl, fmt, ##__VA_ARGS__)

#define rt_sched_err(fmt, ...) rt_sched_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define rt_sched_warn(fmt, ...) rt_sched_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define rt_sched_info(fmt, ...) rt_sched_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define rt_sched_debug(fmt, ...) rt_sched_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define rt_sched_trace(fmt, ...) rt_sched_log(LOG_TRACE, fmt, ##__VA_ARGS__)

/* We'll want to define a structure that contains the operations
 * that real-time schedulers are meant to implement, and then
 * define a structure for statically allocated realtime schedulers
 * to use, that contains the operations, but also other data. */

struct rt_scheduler;
struct rt_scheduler_percpu;

typedef void (*rt_scheduler_fn)(struct rt_scheduler_percpu *);
typedef void (*rt_scheduler_thread_fn)(struct rt_scheduler_percpu *,
                                       struct thread *);

/* This enum defines *what* the realtime scheduler will tell you from
 * functions. For example, when it summarizes itself and produces a
 * `struct rt_thread_summary` it will also send along an error
 * with it, denoting any internal happening, e.g. not being able
 * to migrate threads because of deadline reasons... */
enum rt_scheduler_error {
    /* Negative: Failure w/ message */

    RT_SCHEDULER_OK = 0,

    /* Positive: Success w/ message */
};

struct rt_thread_summary_core {};

struct rt_thread_summary_ext {};

struct rt_scheduler_ops {
    rt_scheduler_fn init;          /* Called at boot time */
    rt_scheduler_fn on_switch_in;  /* When the scheduler is switched in */
    rt_scheduler_fn on_switch_out; /* When the scheduler is switched out */
    struct thread *(*pick_thread)(
        struct rt_scheduler_percpu *); /* select next RT thread, if any */

    rt_scheduler_thread_fn add_thread;
    rt_scheduler_thread_fn remove_thread;

    rt_scheduler_fn on_tick;
};

struct rt_scheduler {
    struct list_head list; /* For list of global RT schedulers */
    struct rt_scheduler_ops ops;
    const char *name;
    void *data;
};

struct rt_scheduler_percpu {
    struct list_head thread_list; /* Threads are placed here when the scheduler
                                     gets switched out */

    struct scheduler *associated_scheduler; /* backpointer to
                                             * `struct scheduler` */

    size_t (*get_rt_thread_count)(struct scheduler *); /* This is stored in the
                                                        * scheduler itself,
                                                        * no need to duplicate
                                                        * the tracking and
                                                        * data into the
                                                        * rt_scheduler */

    /* Keeping track of the rt_thread count is not done by the rt_scheduler.
     * `struct scheduler` will handle this responsibility, and will count
     * whenever the threads are enqueued/dequeued. */

    /* We keep a local copy of the rt_scheduler. This allows us to safely
     * read whatever current rt_scheduler is being used. If the RT scheduler
     * ever gets swapped, we will still read from this variable until
     * we enter the reschedule routine and notice that the RT scheduler has been
     * swapped. Then, we will safely stop this one and switch to the new one */
    struct rt_scheduler *rt_scheduler;
};
