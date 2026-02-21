#include <math/clamp.h>
#include <math/min_max.h>
#include <sch/climb.h>
#include <sch/periodic_work.h>
#include <sch/sched.h>
#include <thread/thread.h>

#include "internal.h"

void climb_per_period_hook();
SCHEDULER_PERIODIC_WORK_REGISTER_PER_PERIOD(climb_per_period_hook,
                                            PERIODIC_WORK_MID);

struct climb_summary {
    climb_pressure_t total_pressure_ewma;
    size_t total_periods_spent;
    size_t nthreads;
};

struct climb_budget {
    int32_t max_boost_levels;
    int32_t remaining;
};

#define CLIMB_EWMA(val, target)                                                \
    (fx_mul(CLIMB_BOOST_EWMA_ALPHA, val) +                                     \
     fx_mul(FX_ONE - CLIMB_BOOST_EWMA_ALPHA, fx_from_int(target)))

static inline struct rbt *climb_tree_local() {
    return &smp_core_scheduler()->climb_threads;
}

/*
 * shaped = p^CLIMB_PRESSURE_EXPONENT
 * level = floor(shaped * CLIMB_BOOST_LEVEL_MAX)
 * boost_clamp(level)
 */
static inline int32_t climb_pressure_to_boost_target(climb_pressure_t p) {
    climb_pressure_t shaped = fx_pow_i32(p, CLIMB_PRESSURE_EXPONENT);

    int32_t level = fx_to_int(fx_mul(shaped, FX(CLIMB_BOOST_LEVEL_MAX)));

    if (level < 0)
        level = 0;

    if (level > CLIMB_BOOST_LEVEL_MAX)
        level = CLIMB_BOOST_LEVEL_MAX;

    return level;
}

/*
 * total_pressure =
 *     pressure_clamp(direct_pressure + indirect_pressure * indirect_weight)
 */
static inline climb_pressure_t
climb_thread_total_pressure(struct climb_thread_state *cts) {
    return fx_clamp(cts->direct_pressure +
                        fx_mul(cts->indirect_pressure, CLIMB_INDIRECT_WEIGHT),
                    0, CLIMB_PRESSURE_MAX);
}

/*
 * boost_ewma = alpha * old + (1 − alpha) * target
 */
static void update_fields(struct climb_thread_state *cts) {
    climb_pressure_t p = climb_thread_total_pressure(cts);
    int target = climb_pressure_to_boost_target(p);

    /* EWMA */
    cts->boost_ewma = CLIMB_EWMA(cts->boost_ewma, target);
    cts->wanted_boost_level = fx_to_int(cts->boost_ewma);
    cts->pressure_ewma = CLIMB_EWMA(
        cts->pressure_ewma, cts->indirect_pressure + cts->direct_pressure);
}

climb_pressure_t climb_thread_get_pressure(struct thread *t) {
    return climb_thread_total_pressure(&t->climb_state);
}

/*
 * new_pressure = p + delta * (max - p)
 */
climb_pressure_t climb_accumulate(climb_pressure_t p, climb_pressure_t delta,
                                  climb_pressure_t max) {
    return p + fx_mul(delta, (max - p));
}

/*
 * scale = max(1 - direct, min_scale)
 */
static inline climb_pressure_t
climb_pressure_scale_indirect(climb_pressure_t direct) {
    climb_pressure_t scale = FX_ONE - direct;
    return fx_max(scale, CLIMB_INDIRECT_MIN_SCALE);
}

/*
 *
 * if direct pressure:
 *     new_direct_pressure = direct_pressure + delta *
 *         (direct_max − direct_pressure)
 * if indirect pressure:
 *     scale = max(1 - direct_pressure, indirect_min_scale)
 *     scaled_delta = delta * scale
 *     new_indirect_pressure = indirect_pressure +
 *                             scaled_delta *
 *                             (indirect_max - indirect_pressure)
 */
static void apply_handle_pressures(struct thread *t, struct climb_handle *ch) {
    struct climb_thread_state *cts = &t->climb_state;

    climb_pressure_t delta = ch->pressure;

    if (t == scheduler_get_current_thread()) {
        kassert(ch->kind == CLIMB_PRESSURE_DIRECT);
        climb_pressure_t old = cts->direct_pressure;

        climb_pressure_t newp =
            climb_accumulate(old, delta, CLIMB_DIRECT_PRESSURE_MAX);

        ch->applied_pressure_internal = newp - old;
        cts->direct_pressure = newp;
        return;
    }
    kassert(ch->kind == CLIMB_PRESSURE_INDIRECT);

    /* indirect pressure */
    climb_pressure_t scale =
        climb_pressure_scale_indirect(cts->direct_pressure);

    climb_pressure_t scaled_delta = fx_mul(delta, scale);

    climb_pressure_t old = cts->indirect_pressure;
    climb_pressure_t newp =
        climb_accumulate(old, scaled_delta, CLIMB_INDIRECT_PRESSURE_MAX);

    ch->applied_pressure_internal = newp - old;
    cts->indirect_pressure = newp;
}

/* This assumes that the thread is already properly locked/protected */
static void apply_handle(struct thread *t, struct climb_handle *ch) {
    kassert(list_empty(&ch->list));

    list_add_tail(&ch->list, &t->climb_state.handles);
    apply_handle_pressures(t, ch);
    struct climb_thread_state *cts = &t->climb_state;
    cpu_id_t cpu = t->curr_core;

    ch->given_by = scheduler_get_current_thread();

    /* Was previously not on tree */
    if (cts->pressure_periods == 0) {
        cts->pressure_periods = 1;
        kassert(!cts->on_climb_tree);
        kassert(rbt_node_empty(&cts->climb_node));
        struct rbt *tree = &global.schedulers[cpu]->climb_threads;
        rbt_insert(tree, &cts->climb_node);
        cts->on_climb_tree = true;
    }
}

static void remove_handle(struct thread *t, struct climb_handle *ch) {
    struct climb_thread_state *cts = &t->climb_state;
    kassert(ch->given_by == scheduler_get_current_thread());

    if (ch->applied_pressure_internal == 0)
        return;

    if (ch->kind == CLIMB_PRESSURE_DIRECT) {
        cts->direct_pressure -= ch->applied_pressure_internal;
    } else {
        kassert(ch->kind == CLIMB_PRESSURE_INDIRECT);
        cts->indirect_pressure -= ch->applied_pressure_internal;
    }

    ch->applied_pressure_internal = 0;
    list_del_init(&ch->list);
}

static void climb_handle_act_self(struct thread *t, struct climb_handle *h,
                                  void (*act)(struct thread *,
                                              struct climb_handle *h)) {
    enum irql irql = IRQL_PASSIVE_LEVEL;
    bool irql_change = false;
    if (irql_get() < IRQL_DISPATCH_LEVEL) {
        irql = irql_raise(IRQL_DISPATCH_LEVEL);
        irql_change = true;
    }

    kassert(t == scheduler_get_current_thread());
    act(t, h);

    if (irql_change)
        irql_lower(irql);
}

static void climb_handle_act_other(struct thread *t, struct climb_handle *ch,
                                   void (*act)(struct thread *,
                                               struct climb_handle *)) {
    /* try and get a ref */
    if (!thread_get(t))
        return;

    /* thread cannot disappear under us */
    enum thread_flags out_flags;
    struct scheduler *sch = thread_get_last_ran(t, &out_flags);
    enum irql irql = spin_lock_irq_disable(&sch->lock);

    act(t, ch);

    spin_unlock(&sch->lock, irql);
    thread_restore_flags(t, out_flags);
    thread_put(t);
}

static bool climb_get_ref(struct thread *t) {
    if (t != scheduler_get_current_thread())
        return thread_get(t);

    return true;
}

static void climb_drop_ref(struct thread *t) {
    if (t != scheduler_get_current_thread())
        thread_put(t);
}

static void climb_handle_act(struct thread *t, struct climb_handle *h,
                             void (*act)(struct thread *,
                                         struct climb_handle *)) {
    if (t == scheduler_get_current_thread()) {
        climb_handle_act_self(t, h, act);
    } else {
        climb_handle_act_other(t, h, act);
    }
}

void climb_handle_remove(struct thread *t, struct climb_handle *h) {
    climb_handle_act(t, h, remove_handle);
    climb_drop_ref(t);
}

void climb_handle_apply(struct thread *t, struct climb_handle *h) {
    if (!climb_get_ref(t))
        return;

    climb_handle_act(t, h, apply_handle);
}

static struct climb_budget climb_budget_from_summary(struct climb_summary *s) {
    struct climb_budget b;

    size_t boost_scale = CLIMB_GLOBAL_BOOST_SCALE(s->nthreads);
    b.max_boost_levels =
        fx_to_int(fx_mul(s->total_pressure_ewma, fx_from_int(boost_scale)));

    int32_t max = s->nthreads * CLIMB_BOOST_LEVELS;
    CLAMP(b.max_boost_levels, CLIMB_MIN_GLOBAL_BOOST, max);

    b.remaining = b.max_boost_levels;
    return b;
}

static void climb_apply_budget(struct scheduler *sched,
                               struct climb_budget *b) {
    struct rbt_node *node;

    rbt_for_each_reverse(node, &sched->climb_threads) {
        if (b->remaining <= 0)
            break;

        struct climb_thread_state *cts =
            climb_thread_state_from_tree_node(node);

        int32_t desired = cts->wanted_boost_level;
        int32_t granted = MIN(desired, b->remaining);

        cts->effective_boost = granted;
        b->remaining -= granted;
    }
}

static struct climb_summary summarize_and_advance(struct rbt *tree) {
    struct climb_summary ret = {0};
    struct climb_thread_state *iter;
    struct rbt_node *node;

    /* Sum it all up */
    rbt_for_each(node, tree) {
        iter = climb_thread_state_from_tree_node(node);
        update_fields(iter);
        ret.nthreads++;

        ret.total_pressure_ewma += iter->pressure_ewma;

        if (iter->pressure_periods > 0)
            ret.total_periods_spent += iter->pressure_periods;
    }

    return ret;
}
void climb_per_period_hook() {
    if (rbt_empty(climb_tree_local()))
        return;

    struct climb_summary summary = summarize_and_advance(climb_tree_local());
    struct climb_budget budget = climb_budget_from_summary(&summary);
    climb_apply_budget(smp_core_scheduler(), &budget);
}

void climb_thread_init(struct thread *t) {
    struct climb_thread_state *cts = &t->climb_state;
    cts->on_climb_tree = false;
    cts->boost_ewma = FX(0);
    cts->wanted_boost_level = 0;
    cts->pressure_periods = 0;
    INIT_LIST_HEAD(&cts->handles);
    cts->direct_pressure = 0;
    cts->indirect_pressure = 0;
    rbt_init_node(&cts->climb_node);
    struct climb_handle *ch = &cts->handle;
    ch->name = t->name;
    ch->applied_pressure_internal = 0;
    ch->kind = CLIMB_PRESSURE_INDIRECT;
    INIT_LIST_HEAD(&ch->list);
}

void climb_post_migrate_hook(struct thread *t, size_t old_cpu, size_t new_cpu) {
    if (rbt_node_empty(&t->climb_state.climb_node)) {
        kassert(t->climb_state.on_climb_tree == false);
        kassert(t->climb_state.pressure_periods == 0);
        return;
    }
    
    /* Locks are already held */
    struct scheduler *old = global.schedulers[old_cpu];
    struct scheduler *new = global.schedulers[new_cpu];

    /* Migrate and recompute */
    rbt_delete(&old->climb_threads, &t->climb_state.climb_node);
    rbt_insert(&new->climb_threads, &t->climb_state.climb_node);
}

size_t climb_get_thread_data(struct rbt_node *n) {
    struct climb_thread_state *cts = climb_thread_state_from_tree_node(n);
    return cts->pressure_periods << CLIMB_PRESSURE_KEY_SHIFT |
           climb_thread_total_pressure(cts);
}
