#include <log.h>
#include <math/min_max.h>
#include <sch/climb.h>
#include <sch/periodic_work.h>
#include <sch/sched.h>
#include <test/export.h>
#include <thread/thread.h>

#include "internal.h"

void climb_per_period_hook();
SCHEDULER_PERIODIC_WORK_REGISTER_PER_PERIOD(climb_per_period_hook,
                                            PERIODIC_WORK_MID);

#ifdef DEBUG_CLIMB
#define CLIMB_FLAGS LOG_SITE_ALL
#else
#define CLIMB_FLAGS LOG_SITE_LEVEL(LOG_ERROR)
#endif

LOG_SITE_DEFINE(climb, .flags = LOG_SITE_DEFAULT,
                .capacity = LOG_SITE_CAPACITY_DEFAULT,
                .enabled_mask = CLIMB_FLAGS,
                .dump_opts = ((struct log_dump_options){.show_tid = true}));

LOG_HANDLE_DEFINE(climb);
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
 *
 *
 *  2000 +-------------------------------------------------------------------+
 *       |*               +                +                +               *|
 *       |** floor((p ** CLIMB_PRESSURE_EXPONENT) * CLIMB_BOOST_LEVEL_MAX) * |
 *       |  *                                                             ** |
 *       |  *                                                             *  |
 *  1500 |-+ **                                                         ** +-|
 *       |    *                                                         *    |
 *       |     **                                                     **     |
 *       |       *                                                   *       |
 *       |       **                                                 **       |
 *  1000 |-+       *                                               *       +-|
 *       |          **                                           **          |
 *       |           **                                         **           |
 *       |             *                                       *             |
 *       |              **                                   **              |
 *   500 |-+             ***                               ***             +-|
 *       |                  *                             *                  |
 *       |                   **                         **                   |
 *       |                     ***                   ***                     |
 *       |                +       ****     +     ****       +                |
 *     0 +-------------------------------------------------------------------+
 *      -10              -5                0                5                10
 *
 * boost_clamp(level)
 */
static int32_t climb_pressure_to_boost_target(climb_pressure_t p) {
    /* The pressure is between 0..1, so this doesn't do much
     * with minor boosts. We have the BOOST_SCALE for config. purposes */

    p = fx_mul(p, CLIMB_PRESSURE_TO_BOOST_SCALE);
    climb_pressure_t shaped = fx_pow_i32(p, CLIMB_PRESSURE_EXPONENT);

    int32_t level = fx_to_int(fx_mul(shaped, FX(CLIMB_BOOST_LEVEL_MAX)));

    CLAMP(level, 0, CLIMB_BOOST_LEVEL_MAX);
    return level;
}
TEST_EXPORT(climb_pressure_to_boost_target);

/*
 * total_pressure =
 *     pressure_clamp(direct_pressure + indirect_pressure * indirect_weight)
 */
static inline climb_pressure_t
climb_thread_total_pressure(struct climb_thread_data *cts) {
    return fx_clamp(cts->direct_pressure +
                        fx_mul(cts->indirect_pressure, CLIMB_INDIRECT_WEIGHT),
                    0, CLIMB_PRESSURE_MAX);
}

/*
 * boost_ewma = alpha * old + (1 − alpha) * target
 */
static void update_fields(struct climb_thread_data *cts) {
    climb_info("Update fields on %p", cts);
    climb_pressure_t p = climb_thread_total_pressure(cts);
    int32_t target = climb_pressure_to_boost_target(p);

    /* EWMA */
    cts->boost_ewma = CLIMB_EWMA(cts->boost_ewma, target);
    cts->wanted_boost = fx_to_int(cts->boost_ewma);
    cts->pressure_ewma = CLIMB_EWMA(cts->pressure_ewma, p);
}

climb_pressure_t climb_thread_applied_pressure(struct thread *t) {
    return climb_thread_total_pressure(&t->climb_data);
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

static size_t climb_count_handles(struct climb_thread_data *cts) {
    size_t agg = 0;
    struct list_node *iter;
    list_for_each(iter, &cts->handles)
        agg++;
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
    struct climb_thread_data *cts = &t->climb_data;

    climb_pressure_t delta = ch->pressure;

    if (t == thread_get_current()) {
        enforce(ch->kind == CLIMB_PRESSURE_DIRECT);
        climb_pressure_t old = cts->direct_pressure;

        climb_pressure_t newp =
            climb_accumulate(old, delta, CLIMB_DIRECT_PRESSURE_MAX);

        ch->applied_pressure_internal = newp - old;
        enforce(newp - old);
        climb_info("Applying pressure %ld to %p", newp - old, cts);
        cts->direct_pressure = newp;
        return;
    }

    enforce(ch->kind == CLIMB_PRESSURE_INDIRECT);

    /* indirect pressure */
    climb_pressure_t scale =
        climb_pressure_scale_indirect(cts->direct_pressure);

    climb_pressure_t scaled_delta = fx_mul(delta, scale);

    climb_pressure_t old = cts->indirect_pressure;
    climb_pressure_t newp =
        climb_accumulate(old, scaled_delta, CLIMB_INDIRECT_PRESSURE_MAX);

    ch->applied_pressure_internal = newp - old;
    enforce(newp - old);
    climb_info("Applying pressure %ld to %p", newp - old, cts);
    cts->indirect_pressure = newp;
}

/* This assumes that the thread is already properly locked/protected */
static void apply_handle(struct thread *t, struct climb_handle *ch) {
    enforce(!list_node_is_linked(&ch->list));

    list_head_push_back(&t->climb_data.handles, &ch->list);
    apply_handle_pressures(t, ch);
    struct climb_thread_data *cts = &t->climb_data;

    climb_info("Apply handle on %p, %zu", cts, climb_count_handles(cts));

#ifdef DEBUG_CLIMB
    /* If there was already a giver, like with indirect boosts, we don't
     * change it. Otherwise, we do, and say we are the giver */
    ch->given_by = ch->given_by ? ch->given_by : thread_get_current();
#endif

    /* Was previously not on tree */
    if (cts->pressure_periods == 0) {
        cts->pressure_periods = 1;
        enforce(!cts->on_climb_tree);
        struct scheduler *sched = thread_get_scheduler_unsafe(t);
        struct rbt *tree = &sched->climb_threads;

        /* Get a reference for the tree */
        enforce(thread_getref(t));
        climb_info("Insert %p to tree", cts);
        rbt_insert(tree, &cts->climb_node);
        cts->on_climb_tree = true;
    } else if (cts->pressure_periods < 0) {
        /* It was previously on decay... all we need to do
         * is tell the thread to start pressure again,
         * and set pressure_periods to 1 */
        cts->pressure_periods = 1;

        /* reinsert upon pressure change */
        if (cts->on_climb_tree) {
            struct scheduler *sched = thread_get_scheduler_unsafe(t);
            rbt_reinsert(&sched->climb_threads, &cts->climb_node);
        }
    }
}

static void remove_handle(struct thread *t, struct climb_handle *ch) {
    struct climb_thread_data *cts = &t->climb_data;

#ifdef DEBUG_CLIMB
    enforce(ch->given_by == thread_get_current());
#endif

    if (ch->applied_pressure_internal == 0) {
        climb_warn("No-op handle removed from %p", cts);
        return;
    }

    if (ch->kind == CLIMB_PRESSURE_DIRECT) {
        cts->direct_pressure -= ch->applied_pressure_internal;
    } else {
        enforce(ch->kind == CLIMB_PRESSURE_INDIRECT);
        cts->indirect_pressure -= ch->applied_pressure_internal;
    }

    ch->applied_pressure_internal = 0;
    list_node_delete_init(&ch->list);

    climb_info("Remove handle on %p (thread %p), %zu left", cts, t,
               climb_count_handles(cts));

    if (list_head_empty(&cts->handles)) {
        /* This thread is done, let it decay now */
        if (cts->on_climb_tree) {
            cts->pressure_periods = -1;
            climb_info("Begin decay on %p", cts);
        }
    }
}

static void climb_handle_act_self(
    struct thread *t, struct climb_handle *h,
    void (*act)(struct thread *, struct climb_handle *h)) TSA_NO_ANALYSIS {
    enum irql irql = IRQL_PASSIVE_LEVEL;
    bool irql_change = false;
    if (irql_get() < IRQL_DISPATCH_LEVEL) {
        irql = irql_raise(IRQL_DISPATCH_LEVEL);
        irql_change = true;
    }

    enforce(t == thread_get_current());
    act(t, h);

    if (irql_change)
        irql_lower(irql);
}

static void climb_handle_act_other(struct thread *t, struct climb_handle *ch,
                                   void (*act)(struct thread *,
                                               struct climb_handle *)) {
    /* thread cannot disappear under us */
    struct scheduler *sch;
    enum irql irql = thread_lock_scheduler(t, &sch);
    act(t, ch);
    spin_unlock(&sch->lock, irql);
}

static bool climb_getref_not_curr(struct thread *t) {
    if (t != thread_get_current())
        return thread_getref(t);

    return true;
}

/* Fine to thread_putref here since we don't hold the scheduler lock */
static void climb_putref_not_curr(struct thread *t) {
    if (t != thread_get_current())
        thread_putref(t);
}

static void climb_handle_act(struct thread *t, struct climb_handle *h,
                             void (*act)(struct thread *,
                                         struct climb_handle *)) {
    if (t == thread_get_current()) {
        climb_handle_act_self(t, h, act);
    } else {
        climb_handle_act_other(t, h, act);
    }
}

static void climb_handle_act_locked(struct thread *t, struct climb_handle *h,
                                    void (*act)(struct thread *,
                                                struct climb_handle *)) {
    if (t == thread_get_current()) {
        climb_handle_act_self(t, h, act);
    } else {
        act(t, h);
    }
}

void climb_handle_apply(struct thread *t, struct climb_handle *h) {
    if (!climb_getref_not_curr(t))
        return;

    climb_handle_act(t, h, apply_handle);
    h->given_to = t;
}

void climb_handle_apply_locked(struct thread *t, struct climb_handle *h) {
    if (!climb_getref_not_curr(t))
        return;

    climb_handle_act_locked(t, h, apply_handle);
    h->given_to = t;
}

void climb_handle_remove(struct climb_handle *h) {
    if (h->applied_pressure_internal == 0) {
        enforce(!list_node_is_linked(&h->list));
        return;
    }

    struct thread *t = h->given_to;
    climb_handle_act(t, h, remove_handle);
    h->given_to = NULL;
    climb_putref_not_curr(t);
}

void climb_handle_remove_locked(struct climb_handle *h) {
    if (h->applied_pressure_internal == 0) {
        enforce(!list_node_is_linked(&h->list));
        return;
    }

    struct thread *t = h->given_to;
    climb_handle_act_locked(t, h, remove_handle);
    h->given_to = NULL;
    climb_putref_not_curr(t);
}

void climb_thread_remove(struct thread *t) {
    struct scheduler *sched;
    enum irql irql_out = thread_lock_scheduler(t, &sched);

    struct climb_thread_data *cts = &t->climb_data;
    struct rbt *tree = &sched->climb_threads;
    struct rbt_node *node = &cts->climb_node;

    bool need_putref = false;
    if (rbt_has_node(tree, node)) {
        climb_info("Removing from tree %p", cts);
        rbt_delete(tree, node);
        cts->pressure_periods = 0;
        cts->on_climb_tree = false;
        need_putref = true;
    }

    spin_unlock(&sched->lock, irql_out);

    /* OK outside of the scheduler lock */
    if (need_putref)
        thread_putref(t);
}

static struct climb_budget climb_budget_from_summary(struct climb_summary *s) {
    struct climb_budget b;

    enforce(s->nthreads);
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

        struct climb_thread_data *cts = climb_thread_data_from_tree_node(node);

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

/* Threads dropped from the tree per period. NOTE: that this can
 * only happen AFTER the scheduler lock is dropped,
 * since the last thread_putref() can wake the reaper.
 *
 * TODO: There's probably a better way */
#define CLIMB_DROP_BATCH 64

struct climb_drop_batch {
    struct thread *threads[CLIMB_DROP_BATCH];
    size_t count;
};

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
static void maybe_remove_node(struct rbt *tree, struct climb_thread_data *cts,
                              struct climb_drop_batch *drop) {
    struct rbt_node *node = &cts->climb_node;
    bool remove = false;

    if (cts->pressure_periods < -CLIMB_MAX_DECAY_PERIODS)
        remove = true;

    if (cts->wanted_boost == 0)
        remove = true;

    if (remove) {
        /* Leave it on tree for next period */
        if (drop->count == CLIMB_DROP_BATCH)
            return;

        climb_info("Removing from tree %p", cts);
        rbt_delete(tree, node);
        cts->pressure_periods = 0;
        cts->on_climb_tree = false;
        drop->threads[drop->count++] =
            container_of(cts, struct thread, climb_data);
    } else {
        cts->pressure_periods--;
    }
}

static struct climb_summary
summarize_and_advance(struct rbt *tree, struct climb_drop_batch *drop) {
    struct climb_summary ret = {0};
    struct climb_thread_data *iter;
    struct rbt_node *node, *tmp;

    /* Sum it all up */
    rbt_for_each_safe(node, tmp, tree) {
        iter = climb_thread_data_from_tree_node(node);
        update_fields(iter);
        ret.nthreads++;

        ret.total_pressure_ewma += iter->pressure_ewma;

        if (iter->pressure_periods > 0) {
            ret.total_periods_spent += iter->pressure_periods;
            iter->pressure_periods++;
        } else {
            maybe_remove_node(tree, iter, drop);
        }
    }

    return ret;
}

void climb_per_period_hook() {
    struct scheduler *sched = smp_core_scheduler();
    enum irql irql = spin_lock_high(&sched->lock);

    if (rbt_empty(climb_tree_local())) {
        spin_unlock(&sched->lock, irql);
        return;
    }

    struct climb_drop_batch drop = {.count = 0};

    struct climb_summary summary =
        summarize_and_advance(climb_tree_local(), &drop);
    struct climb_budget budget = climb_budget_from_summary(&summary);
    climb_apply_budget(smp_core_scheduler(), &budget);

    spin_unlock(&sched->lock, irql);

    for (size_t i = 0; i < drop.count; i++)
        thread_putref(drop.threads[i]);
}

void climb_thread_init(struct thread *t) {
    struct climb_thread_data *cts = &t->climb_data;
    cts->on_climb_tree = false;
    cts->boost_ewma = FX(0);
    cts->wanted_boost = 0;
    cts->pressure_periods = 0;
    list_head_init(&cts->handles);
    cts->direct_pressure = 0;
    cts->indirect_pressure = 0;
    rbt_init_node(&cts->climb_node);
    struct climb_handle *ch = &cts->handle;
    ch->applied_pressure_internal = 0;
    ch->kind = CLIMB_PRESSURE_INDIRECT;

#ifdef DEBUG_CLIMB
    ch->given_by = t;
#endif

    ch->given_to = NULL;
    list_node_init(&ch->list);
}

void climb_post_migrate_hook(struct thread *t, size_t old_cpu, size_t new_cpu) {
    /* Locks are already held */
    struct scheduler *old = global.schedulers[old_cpu];
    struct scheduler *new = global.schedulers[new_cpu];

    if (!rbt_has_node(&old->climb_threads, &t->climb_data.climb_node)) {
        enforce(t->climb_data.on_climb_tree == false,
                "thread %s is off cpu %zu's tree but thinks it is on one",
                t->name, old_cpu);
        enforce(t->climb_data.pressure_periods == 0,
                "thread %s left the tree carrying pressure_periods=%d", t->name,
                t->climb_data.pressure_periods);
        return;
    }

    climb_warn("Migrating %p", &t->climb_data);

    /* Migrate and recompute */
    rbt_move(&old->climb_threads, &new->climb_threads,
             &t->climb_data.climb_node);
}

/* This is how we key our red black tree.
 *
 * Our 64 bit fixed point representation uses the upper dword as the "integer"
 * component and the lower dword as the "decimal" part. However, we
 * also sort wrt. the amount of elapsed periods to avoid favoring
 * high pressure recent threads over lower pressure older threads
 *
 * 63.. .... .... .... .... .... .... .... .... .... .... .... .... ...0
 *                                 S
 *
 * "S" is the lowest bit of pressure_periods, at CLIMB_PRESSURE_KEY_SHIFT.
 * Thus, one period is 0.5 pressure points.
 *

 * The key has to be a signed 64-bit integer. Full pressure points don't fit
 * in 32 bits, and pressure_periods can become negative as threads decay.
 */

static int64_t climb_key(struct climb_thread_data *cts) {
    return (int64_t) cts->pressure_periods * (1LL << CLIMB_PRESSURE_KEY_SHIFT) +
           climb_thread_total_pressure(cts);
}

/* rbt_get_data is an unsigned key */
size_t climb_get_thread_data(struct rbt_node *n) {
    int64_t key = climb_key(climb_thread_data_from_tree_node(n));
    return (size_t) ((uint64_t) key ^ (UINT64_C(1) << 63));
}

int32_t climb_cmp_threads(const struct rbt_node *a, const struct rbt_node *b) {
    int64_t ka =
        climb_key(climb_thread_data_from_tree_node((struct rbt_node *) a));
    int64_t kb =
        climb_key(climb_thread_data_from_tree_node((struct rbt_node *) b));

    return (ka > kb) - (ka < kb);
}
