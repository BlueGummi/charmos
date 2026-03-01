/* @title: CLIMB framework */
#pragma once
#include <math/fixed.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <structures/rbt.h>
#include <thread/thread_types.h>

typedef fx16_16_t climb_pressure_t;
struct climb_handle;

#define CLIMB_BOOST_LEVELS 20
#define CLIMB_MIN_GLOBAL_BOOST 1
#define CLIMB_REINSERT_THRESHOLD 2
#define CLIMB_GLOBAL_BOOST_SCALE(nthread) (CLIMB_BOOST_LEVELS / nthread)
#define CLIMB_PRESSURE_KEY_SHIFT 15
#define CLIMB_MAX_DECAY_PERIODS 20

enum climb_pressure_kind {
    CLIMB_PRESSURE_DIRECT,
    CLIMB_PRESSURE_INDIRECT,
};

struct climb_handle {
    char *name;
    struct list_head list;
    climb_pressure_t pressure;
    climb_pressure_t applied_pressure_internal; /* If 0, this has not
                                                 * applied presssure */

    enum climb_pressure_kind kind;

    struct climb_source *pressure_source;
    struct thread *given_by; /* Debugging */
    struct thread *given_to;
};

struct climb_thread_state {
    /* pressure */
    climb_pressure_t direct_pressure;   /* Contributed direct pressure */
    climb_pressure_t indirect_pressure; /* Indirect pressure from others */
    climb_pressure_t pressure_ewma;     /* EWMA of pressure over time */

    /* boost */
    int32_t wanted_boost;    /* 0..20, how much this thread "wants" */
    fx16_16_t boost_ewma;    /* EWMA of the wanted_boost_raw */
    int32_t effective_boost; /* How much boosted (derived from wanted_boost_raw)
                              * this thread "gets". This exists because CLIMB
                              * will take into account the existence of other
                              * threads and their boosts and periods spent
                              * to compute the boost of one thread */

    /* time */
    int32_t pressure_periods; /* >=1 active, < 0 decaying, 0 inactive */

    /* accounting */
    struct list_head handles; /* active pressure handles */

    /* scheduler integration */
    bool on_climb_tree; /* Used to logically verify things */
    struct rbt_node climb_node;

    /* if this thread becomes a CLIMB source,
     * what would it "apply"? */
    struct climb_handle handle;
};
#define climb_thread_state_from_tree_node(tn)                                  \
    (container_of(tn, struct climb_thread_state, climb_node))

struct climb_source {
    char *name;
    climb_pressure_t base;
};

/* Pressures */
#define CLIMB_PRESSURE_THREAD_BASE FX(0.05)
#define CLIMB_PRESSURE_IO_BASE FX(0.20)
#define CLIMB_PRESSURE_LOCK_BASE FX(0.10)
#define CLIMB_PRESSURE_MAX FX(1.0)
#define CLIMB_PRESSURE(x) FX(x)

/* Pressure space */
#define CLIMB_PRESSURE_MAX FX(1.0)
#define CLIMB_DIRECT_PRESSURE_MAX FX(1.0)
#define CLIMB_INDIRECT_PRESSURE_MAX FX(1.0)

/* Indirect pressure scaling */
#define CLIMB_INDIRECT_MIN_SCALE FX(0.10)
#define CLIMB_INDIRECT_WEIGHT FX(0.85)

/* Boost space */
#define CLIMB_BOOST_LEVEL_MAX 20

/* EWMA smoothing */
#define CLIMB_BOOST_EWMA_ALPHA FX(0.75)

/* Pressure to boost shaping */
#define CLIMB_PRESSURE_EXPONENT 3 /* cubic */
#define CLIMB_PRESSURE_TO_BOOST_SCALE FX(8.0)

#define CLIMB_SOURCE_EXTERN(name) extern struct climb_source __climb_src_##name
#define CLIMB_SOURCE_CREATE(n, strname, b)                                     \
    struct climb_source __climb_src_##n = {.name = strname, .base = b}
#define CLIMB_SOURCE(name) &(__climb_src_##name)

climb_pressure_t climb_thread_applied_pressure(struct thread *t);
climb_pressure_t climb_thread_compute_pressure_to_apply(struct thread *t);

void climb_handle_apply(struct thread *t, struct climb_handle *h);
void climb_handle_update(struct thread *t, struct climb_handle *h,
                         climb_pressure_t new_pressure);
void climb_handle_remove(struct climb_handle *h);

void climb_handle_apply_locked(struct thread *t, struct climb_handle *h);
void climb_handle_remove_locked(struct climb_handle *h);

void climb_recompute_pressure(struct thread *t);
void climb_thread_init(struct thread *t);
void climb_post_migrate_hook(struct thread *t, size_t old_cpu, size_t new_cpu);
size_t climb_get_thread_data(struct rbt_node *n);

static inline struct climb_handle *
climb_handle_init(struct climb_handle *ch, struct climb_source *cs,
                  enum climb_pressure_kind k) {
    INIT_LIST_HEAD(&ch->list);
    if (cs) {
        ch->pressure = cs->base;
        ch->name = cs->name;
        ch->pressure_source = cs;
    }

    ch->kind = k;
    ch->applied_pressure_internal = 0;
    return ch;
}
