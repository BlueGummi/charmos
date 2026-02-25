#include <log.h>
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

LOG_SITE_DECLARE(climb, LOG_SITE_PRINT | LOG_SITE_DEFAULT,
                 LOG_SITE_CAPACITY_DEFAULT, LOG_SITE_ALL,
                 ((struct log_dump_opts) {.show_tid = true,
                                          .show_args = true}));
LOG_HANDLE_DECLARE_DEFAULT(climb);
#define climb_log(lvl, fmt, ...)                                               \
    log(LOG_SITE(climb), LOG_HANDLE(climb), lvl, fmt, ##__VA_ARGS__)

#define climb_err(fmt, ...) climb_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define climb_warn(fmt, ...) climb_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define climb_info(fmt, ...) climb_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define climb_debug(fmt, ...) climb_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define climb_trace(fmt, ...) climb_log(LOG_TRACE, fmt, ##__VA_ARGS__)

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
 * shaped = p ^ CLIMB_PRESSURE_EXPONENT
 * level = floor(shaped * CLIMB_BOOST_LEVEL_MAX)
 * boost_clamp(level)
 */
static inline int32_t climb_pressure_to_boost_target(climb_pressure_t p) {
    climb_pressure_t shaped = fx_pow_i32(p, CLIMB_PRESSURE_EXPONENT);

    int32_t level = fx_to_int(fx_mul(shaped, FX(CLIMB_BOOST_LEVEL_MAX)));
    CLAMP(level, 0, CLIMB_BOOST_LEVEL_MAX);

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
    climb_info("Update fields on 0x%lx", cts);
    climb_pressure_t p = climb_thread_total_pressure(cts);
    int32_t target = climb_pressure_to_boost_target(p);

    /* EWMA */
    cts->boost_ewma = CLIMB_EWMA(cts->boost_ewma, target);
    cts->wanted_boost = fx_to_int(cts->boost_ewma);
    cts->pressure_ewma = CLIMB_EWMA(
        cts->pressure_ewma, cts->indirect_pressure + cts->direct_pressure);
}

climb_pressure_t climb_thread_applied_pressure(struct thread *t) {
    return climb_thread_total_pressure(&t->climb_state);
}

climb_pressure_t climb_thread_compute_pressure_to_apply(struct thread *t) {
    return CLIMB_PRESSURE_THREAD_BASE + climb_thread_applied_pressure(t);
    ;
}

/*
 * new_pressure = p + delta * (max - p)
 */
static inline climb_pressure_t climb_accumulate(climb_pressure_t p,
                                                climb_pressure_t delta,
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

static size_t climb_count_handles(struct climb_thread_state *cts) {
    size_t agg = 0;
    struct list_head *iter;
    list_for_each(iter, &cts->handles) agg++;
    return agg;
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

    if (t == thread_get_current()) {
        kassert(ch->kind == CLIMB_PRESSURE_DIRECT);
        climb_pressure_t old = cts->direct_pressure;

        climb_pressure_t newp =
            climb_accumulate(old, delta, CLIMB_DIRECT_PRESSURE_MAX);

        ch->applied_pressure_internal = newp - old;
        kassert(newp - old);
        climb_info("Applying pressure %u to 0x%lx", newp - old, cts);
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
    kassert(newp - old);
    climb_info("Applying pressure %u to 0x%lx", newp - old, cts);
    cts->indirect_pressure = newp;
}

/* This assumes that the thread is already properly locked/protected */
static void apply_handle(struct thread *t, struct climb_handle *ch) {
    kassert(list_empty(&ch->list));

    list_add_tail(&ch->list, &t->climb_state.handles);
    apply_handle_pressures(t, ch);
    struct climb_thread_state *cts = &t->climb_state;

    climb_info("Apply handle on 0x%lx, %u", cts, climb_count_handles(cts));

    /* If there was already a giver, like with indirect boosts, we don't
     * change it. Otherwise, we do, and say we are the giver */
    ch->given_by = ch->given_by ? ch->given_by : thread_get_current();

    /* Was previously not on tree */
    if (cts->pressure_periods == 0) {
        cts->pressure_periods = 1;
        kassert(!cts->on_climb_tree);
        enum thread_flags out;
        struct scheduler *sched = thread_get_last_ran(t, &out);
        struct rbt *tree = &sched->climb_threads;

        /* Get a reference for the tree */
        kassert(thread_get(t));
        climb_info("Insert 0x%lx to tree", cts);
        rbt_insert(tree, &cts->climb_node);
        cts->on_climb_tree = true;
        thread_restore_flags(t, out);
    }
}

static void remove_handle(struct thread *t, struct climb_handle *ch) {
    struct climb_thread_state *cts = &t->climb_state;
    kassert(ch->given_by == thread_get_current());

    if (ch->applied_pressure_internal == 0) {
        climb_warn("No-op handle removed from 0x%lx", cts);
        return;
    }

    if (ch->kind == CLIMB_PRESSURE_DIRECT) {
        cts->direct_pressure -= ch->applied_pressure_internal;
    } else {
        kassert(ch->kind == CLIMB_PRESSURE_INDIRECT);
        cts->indirect_pressure -= ch->applied_pressure_internal;
    }

    ch->applied_pressure_internal = 0;
    list_del_init(&ch->list);
    climb_info("Remove handle on 0x%lx (thread 0x%lx), %u left", cts, t,
               climb_count_handles(cts));

    if (list_empty(&cts->handles)) {
        /* This thread is done. Let it decay now */
        cts->pressure_periods = -1;
        climb_info("Begin decay on 0x%lx", cts);
    }
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

    kassert(t == thread_get_current());
    act(t, h);

    if (irql_change)
        irql_lower(irql);
}

static void climb_handle_act_other(struct thread *t, struct climb_handle *ch,
                                   void (*act)(struct thread *,
                                               struct climb_handle *),
                                   bool lock) {

    /* thread cannot disappear under us */
    enum thread_flags out_flags;
    struct scheduler *sch = thread_get_last_ran(t, &out_flags);
    enum irql irql;

    if (!lock)
        irql = spin_lock_irq_disable(&sch->lock);

    act(t, ch);

    if (!lock)
        spin_unlock(&sch->lock, irql);

    thread_restore_flags(t, out_flags);
}

static bool climb_get_ref(struct thread *t) {
    if (t != thread_get_current())
        return thread_get(t);

    return true;
}

static void climb_drop_ref(struct thread *t) {
    if (t != thread_get_current())
        thread_put(t);
}

static void climb_handle_act(struct thread *t, struct climb_handle *h,
                             void (*act)(struct thread *,
                                         struct climb_handle *),
                             bool lock) {
    if (t == thread_get_current()) {
        climb_handle_act_self(t, h, act);
    } else {
        climb_handle_act_other(t, h, act, lock);
    }
}

static void climb_handle_remove_internal(struct climb_handle *h, bool lock) {
    /* We might not always have a ref to the thread here.
     * If the handle we are given is completely unused, this is because
     * the thread we are removing from is NOT a timesharing thread.
     *
     * This means we can safely leave and no-op */
    if (h->applied_pressure_internal == 0) {
        kassert(list_empty(&h->list));
        return;
    }

    struct thread *t = h->given_to;
    climb_handle_act(t, h, remove_handle, lock);
    h->given_to = NULL;
    climb_drop_ref(t);
}

static void climb_handle_apply_internal(struct thread *t,
                                        struct climb_handle *h, bool lock) {
    if (!climb_get_ref(t))
        return;

    climb_handle_act(t, h, apply_handle, lock);
    h->given_to = t;
    climb_info("Assign given_to 0x%lx", t);
}

void climb_handle_apply(struct thread *t, struct climb_handle *h) {
    climb_handle_apply_internal(t, h, false);
}

void climb_handle_apply_locked(struct thread *t, struct climb_handle *h) {
    climb_handle_apply_internal(t, h, true);
}

void climb_handle_remove(struct climb_handle *h) {
    climb_handle_remove_internal(h, false);
}

void climb_handle_remove_locked(struct climb_handle *h) {
    climb_handle_remove_internal(h, true);
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

        /* NOTE: threads can get boosted around but we keep them in CLIMB.
         * This is to allow them to still maintain their boosts after they
         * return to TS, however, we still boost any threads within CLIMB,
         * and treat them as if they are all TS threads to allow for a smooth
         * return once a boosted thread comes back to being TS */

        int32_t desired = cts->wanted_boost;
        int32_t granted = MIN(desired, b->remaining);

        cts->effective_boost = granted;
        b->remaining -= granted;
    }
}

/* This is our decay policy after a thread is removed from CLIMB.
 *
 * Because the boost is represented in part by an EWMA, it doesn't
 * sharply drop when all pressure is released, but rather, gradually
 * decays. This allows us to have smoother boost periods of threads,
 * to prevent threads from switching between priority zones and
 * inflicting costs on latency and consistency.
 *
 * Our policy for boost decay is as follows:
 *     When a thread has all of its pressure sources removed,
 *     it sets `pressure_periods` to -1.
 *
 *     Upon subsequent passes within CLIMB's per-period work,
 *     this number decays by one. For example, if a thread has
 *     spent two periods in decay, it would be -2.
 *
 *     Eventually, a thread will lose all of its boost, and
 *     when this happens (i.e. when `wanted_boost` drops to 0),
 *     the thread is removed from CLIMB accounting.
 *
 *     However, there will come a time where too many periods have
 *     elapsed under decay. When this happens, the thread is forcibly
 *     removed. (CLIMB_MAX_DECAY_PERIODS)
 *
 */
static void maybe_remove_node(struct rbt *tree,
                              struct climb_thread_state *cts) {
    struct rbt_node *node = &cts->climb_node;
    bool remove = false;

    if (cts->pressure_periods < -CLIMB_MAX_DECAY_PERIODS)
        remove = true;

    if (cts->wanted_boost == 0)
        remove = true;

    if (remove) {
        climb_info("Removing from tree 0x%lx", cts);
        rbt_delete(tree, node);
        cts->pressure_periods = 0;
        cts->on_climb_tree = false;
        thread_put(container_of(cts, struct thread, climb_state));
    }
}

static struct climb_summary summarize_and_advance(struct rbt *tree) {
    struct climb_summary ret = {0};
    struct climb_thread_state *iter;
    struct rbt_node *node, *tmp;

    /* Sum it all up */
    rbt_for_each_safe(node, tmp, tree) {
        iter = climb_thread_state_from_tree_node(node);
        update_fields(iter);
        ret.nthreads++;

        ret.total_pressure_ewma += iter->pressure_ewma;

        if (iter->pressure_periods > 0) {
            ret.total_periods_spent += iter->pressure_periods;
        } else {
            maybe_remove_node(tree, iter);
        }
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
    cts->wanted_boost = 0;
    cts->pressure_periods = 0;
    INIT_LIST_HEAD(&cts->handles);
    cts->direct_pressure = 0;
    cts->indirect_pressure = 0;
    rbt_init_node(&cts->climb_node);
    struct climb_handle *ch = &cts->handle;
    ch->name = t->name;
    ch->applied_pressure_internal = 0;
    ch->kind = CLIMB_PRESSURE_INDIRECT;
    ch->given_by = t;
    ch->given_to = NULL;
    ch->pressure_source = NULL;
    INIT_LIST_HEAD(&ch->list);
}

void climb_post_migrate_hook(struct thread *t, size_t old_cpu, size_t new_cpu) {
    /* Locks are already held */
    struct scheduler *old = global.schedulers[old_cpu];
    struct scheduler *new = global.schedulers[new_cpu];
    climb_warn("Maybe 0x%lx", &t->climb_state);

    if (!rbt_has_node(&old->climb_threads, &t->climb_state.climb_node)) {
        kassert(t->climb_state.on_climb_tree == false);
        kassert(t->climb_state.pressure_periods == 0);
        return;
    }

    climb_warn("Migrating 0x%lx", &t->climb_state);

    /* Migrate and recompute */
    rbt_delete(&old->climb_threads, &t->climb_state.climb_node);
    rbt_insert(&new->climb_threads, &t->climb_state.climb_node);
}

/* This is how we key our red black tree.
 *
 * 31.. .... .... .... .... .... .... ...0
 *
 *
 * Our 32 bit fixed point representation uses the upper word as the "integer"
 * part and the lower word as the "decimal" part. This is how climb_pressure_t
 * is represented. However, we want to sort our threads in this tree as not
 * just a climb_pressure_t, but also with their amount of elapsed periods.
 *
 * This is because sorting based on just climb_pressure_t will favor high
 * pressure threads, which can potentially starve lower pressure threads
 * that have been waiting for longer periods of time from a boost.
 *
 * climb_pressure_t is only ever a value between 0 and 1 in fixed point,
 * thus we take the approach of shifting in the pressure_periods by
 * a certain shift so that it contributes to the ordering of the tree.
 *
 * 31.. .... .... .... .... .... .... ...0
 *                     S
 *
 * The "S" represents where the lowest bit of the pressure periods would be
 * placed. This effectively means that every period of elapsed pressure
 * is equal to 0.5 climb_pressure_t points, and means that two periods
 * would result in a single maximum climb_pressure_t of pressure.
 */
size_t climb_get_thread_data(struct rbt_node *n) {
    struct climb_thread_state *cts = climb_thread_state_from_tree_node(n);
    return cts->pressure_periods * (1 << CLIMB_PRESSURE_KEY_SHIFT) +
           climb_thread_total_pressure(cts);
}
