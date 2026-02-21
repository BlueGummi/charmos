/* @title: Scheduler Periodic Work */
#pragma once
#include <compiler.h>
#include <structures/list.h>
#include <structures/pairing_heap.h>
#include <types/types.h>

enum scheduler_periodic_work_prio {
    PERIODIC_WORK_HIGH,
    PERIODIC_WORK_MID,
    PERIODIC_WORK_LOW,
    PERIODIC_WORK_MAX,
};

enum scheduler_periodic_work_type {
    PERIODIC_WORK_PERIOD_BASED,
    PERIODIC_WORK_TIME_BASED,
};

struct scheduler_periodic_work_linker_object {
    char *name;
    void (*fn)();

    union {
        time_t time_interval;
        size_t period_interval; /* overloaded */
        size_t interval;
    };

    enum scheduler_periodic_work_type type;
    enum scheduler_periodic_work_prio prio;
} __linker_aligned;

struct scheduler_periodic_work {
    enum scheduler_periodic_work_type type;
    enum scheduler_periodic_work_prio prio;
    char *name;
    void (*fn)();
    cpu_id_t cpu; /* for which CPU? */

    union {
        struct {
            uint64_t last_period_ran;
            uint64_t period_interval;
            uint64_t expected_period;
        };

        struct {
            time_t last_time_ran;
            time_t time_interval;
            time_t expected_next_time;
        };

        /* generic struct - do not reorder members */
        struct {
            size_t last_occurrence;
            size_t interval;
            size_t expected_next;
        };
    };

    size_t executed_times;

    /* irrelevant for non-time based work */
    time_t interval_total_loss; /* all time lost from misses */
    size_t interval_latency;    /* total_loss / executed_times */

    struct pairing_node pnode; /* ordered by expected_next */
};

struct scheduler_periodic_work_limits {
    /* per_call here is each time the works are invoked (together) */
    size_t max_execs_per_call;
    time_t max_duration_per_call_ns;
};

struct scheduler_periodic_work_percpu {
    struct pairing_heap period_based_works[PERIODIC_WORK_MAX];
    struct pairing_heap time_based_works[PERIODIC_WORK_MAX];

    size_t period_based_work_count[PERIODIC_WORK_MAX];
    size_t time_based_work_count[PERIODIC_WORK_MAX];

    struct scheduler_periodic_work_limits limits;
    cpu_id_t cpu;
    bool executing;
};

extern struct scheduler_periodic_work_linker_object
    __skernel_sched_periodic_work[],
    __ekernel_sched_periodic_work[];

#define SCHEDULER_PERIODIC_WORK_REGISTER(_fn, _type, _prio, _interval)         \
    static struct scheduler_periodic_work_linker_object __spw_##_fn            \
        __attribute__((section(".kernel_sched_periodic_work"), used)) = {      \
            .name = #_fn,                                                      \
            .fn = _fn,                                                         \
            .type = _type,                                                     \
            .interval = _interval,                                             \
            .prio = _prio}

#define SCHEDULER_PERIODIC_WORK_REGISTER_PER_PERIOD(_fn, _prio)                \
    SCHEDULER_PERIODIC_WORK_REGISTER(_fn, PERIODIC_WORK_PERIOD_BASED, _prio, 1)

void scheduler_periodic_work_init(void);
void scheduler_periodic_work_execute(enum scheduler_periodic_work_type type);

/* prevent irql_lower work execution recursion */
bool scheduler_in_periodic_work();
