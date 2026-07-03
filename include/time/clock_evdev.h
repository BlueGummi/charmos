/* @title: Clock Event Device */
#pragma once
#include <errno.h>
#include <smp/domain.h>
#include <structures/list.h>
#include <time/clock.h>
#include <types/types.h>

enum clock_evdev_state {
    CLOCK_EVDEV_STATE_OFF,
    CLOCK_EVDEV_STATE_PERIODIC,
    CLOCK_EVDEV_STATE_ONESHOT,
    CLOCK_EVDEV_STATE_ONESHOT_STOPPED,
};

enum clock_evdev_flags {
    CLOCK_EVDEV_PERIODIC = 1,
    CLOCK_EVDEV_ONESHOT = 1 << 1,
    CLOCK_EVDEV_DYNIRQ = 1 << 2,
    CLOCK_EVDEV_PERCPU = 1 << 3,
};

struct clock_evdev {
    char *name;
    enum errno (*set_next_event)(struct clock_evdev *, time_t delta_ns);
    timestamp_t next_event;

    time_t min_delta_ns;
    time_t max_delta_ns;

    enum clock_evdev_state state;
    enum clock_evdev_flags flags;
    enum clock_rating rating;

    enum errno (*change_state)(struct clock_evdev *,
                               enum clock_evdev_state state);
    enum errno (*resume_tick)(struct clock_evdev *);

    size_t min_delta_ticks;
    size_t max_delta_ticks;

    fx32_32_t mult; /* Tick -> NS */

    struct list_head list_internal;
    cpu_id_t bound_to_cpu;
    struct cpu_mask cpu_mask;

    irq_t irq;
    struct clock *clock; /* Optional pointer */
    void *private;
};
