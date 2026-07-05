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
    CLOCK_EVDEV_TICK_SUITABLE = 1 << 4,
};

enum clock_evdev_group_flags {
    CLOCK_EVDEV_GROUP_PERCPU = 1, /* Each CPU has one */
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

    struct list_head list_internal; /* Used in global list */
    struct list_head group_list;    /* struct clock_evdev_group */
    cpu_id_t bound_to_cpu;          /* If CPU_ID_NONE, no binding */
    struct cpu_mask cpu_mask;

    irq_t irq;
    struct clock *clock; /* Optional pointer */
    void *private;
};

/* The clock evdevs are meant to be sequentially added to this */
struct clock_evdev_group {
    char *name;
    struct list_head clock_evdevs;
    enum clock_evdev_group_flags flags;
    struct list_head list_internal; /* Global list as well */
};

struct clock_evdev_group *clock_evdev_group_create(const char *fmt, ...);
struct clock_evdev *clock_evdev_create(const char *fmt, ...);
void clock_evdev_register(struct clock_evdev *ced);
void clock_evdev_group_register(struct clock_evdev_group *cedg);

static inline void clock_evdev_group_add(struct clock_evdev_group *cedg,
                                         struct clock_evdev *ced) {
    list_add(&ced->group_list, &cedg->clock_evdevs);
}
