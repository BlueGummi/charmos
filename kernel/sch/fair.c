#include <asm.h>
#include <boot/stage.h>
#include <crypto/prng.h>
#include <int/idt.h>
#include <mp/mp.h>
#include <registry.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <tests.h>

static void derive_timeshare_prio_range(enum thread_activity_class cls,
                                        uint32_t *min, uint32_t *max);
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define THREAD_DELTA_UNIT (1ULL << 16)

#define THREAD_BOOST_WAKE_SMALL                                                \
    (4ULL * THREAD_DELTA_UNIT) /* provisional boost for wake */

#define THREAD_BOOST_WAKE_LARGE                                                \
    (12ULL * THREAD_DELTA_UNIT) /* stronger boost if very interactive */

#define THREAD_PENALTY_CPU_RUN                                                 \
    (3ULL * THREAD_DELTA_UNIT) /* penalize heavy CPU usage per run period */

#define THREAD_DELTA_MAX_UNITS 0x200

#define THREAD_DELTA_MAX_U64                                                   \
    (THREAD_DELTA_UNIT * (uint64_t) THREAD_DELTA_MAX_UNITS)

#define THREAD_DELTA_MAX                                                       \
    ((THREAD_DELTA_MAX_U64 > (uint64_t) INT32_MAX)                             \
         ? INT32_MAX                                                           \
         : (int32_t) THREAD_DELTA_MAX_U64)

#define THREAD_REINSERT_THRESHOLD                                              \
    (8ULL * THREAD_DELTA_UNIT) /* only reinsert if effective priority changes  \
                               >= this */

#define THREAD_HYSTERESIS_MS 250ULL
#define THREAD_PROV_BOOST_MS 50ULL /* provisional boost expiration */

#define THREAD_MUL_INTERACTIVE 5ULL /* Big boost */
#define THREAD_MUL_IO_BOUND 3ULL    /* Small boost */
#define THREAD_MUL_CPU_BOUND 1ULL   /* No boost */
#define THREAD_MUL_SLEEPY 1ULL      /* No boost */

/* Scheduling periods */
#define MIN_PERIOD_MS 20ULL  /* don’t go too short */
#define MAX_PERIOD_MS 300ULL /* don’t go too long */
#define BASE_PERIOD_MS 50ULL /* baseline for small loads */

/* Timeslices */
#define MIN_SLICE_MS 2  /* smallest slice granularity */
#define MAX_SLICE_MS 20 /* cap slice length to avoid hogs */

#define WAKE_FREQ_MAX 20
#define WAKE_FREQ_SCALE 5
#define BIAS_SCALE_NUM 1
#define BIAS_SCALE_DEN 8

#define SET_MUL(__multiplier)                                                  \
    class_mul = __multiplier;                                                  \
    break;

#define CLAMP(__var, __min, __max)                                             \
    if (__var > __max)                                                         \
        __var = __max;                                                         \
    if (__var < __min)                                                         \
        __var = __min;

static enum thread_activity_class
classify_activity(struct thread_activity_metrics m) {
    if (m.run_ratio > 80 && m.block_ratio < 10)
        return THREAD_ACTIVITY_CLASS_CPU_BOUND;

    if (m.block_ratio > 40 && m.wake_freq > 2)
        return THREAD_ACTIVITY_CLASS_IO_BOUND;

    if (m.wake_freq > 5)
        return THREAD_ACTIVITY_CLASS_INTERACTIVE;

    if (m.sleep_ratio > 50)
        return THREAD_ACTIVITY_CLASS_SLEEPY;

    /* Let's not confuse the rest of the scheduler
     * with an unknown classification */
    return THREAD_ACTIVITY_CLASS_CPU_BOUND;
}

void thread_classify_activity(struct thread *t, uint64_t now_ms) {
    /* Rate limit to prevent rapid bouncing */
    if (now_ms - t->last_class_change_ms < THREAD_HYSTERESIS_MS)
        return;

    t->activity_class = classify_activity(t->activity_metrics);
    t->last_class_change_ms = now_ms;
}

static struct thread_activity_metrics calc_activity_metrics(struct thread *t) {
    struct thread_activity_metrics m = {0};
    uint64_t total_duration = 0;
    uint64_t total_block = 0;
    uint64_t total_sleep = 0;
    uint64_t total_run = 0;
    uint64_t total_wake_count = 0;

    for (size_t i = 0; i < THREAD_ACTIVITY_BUCKET_COUNT; i++) {
        struct thread_activity_bucket *b = &t->activity_stats->buckets[i];
        struct thread_runtime_bucket *rtb = &t->activity_stats->rt_buckets[i];

        uint64_t run_time = rtb->run_time_ms;
        uint64_t block_time = b->block_duration;
        uint64_t sleep_time = b->sleep_duration;

        total_run += run_time;
        total_block += block_time;
        total_sleep += sleep_time;
        total_wake_count += b->wake_count;
    }

    total_duration = total_run + total_block + total_sleep;
    if (total_duration == 0)
        total_duration = 1;

    m.run_ratio = (total_run * 100) / total_duration;
    m.block_ratio = (total_block * 100) / total_duration;
    m.sleep_ratio = (total_sleep * 100) / total_duration;

    m.wake_freq = total_wake_count / THREAD_ACTIVITY_BUCKET_COUNT;

    return m;
}

void thread_calculate_activity_data(struct thread *t) {
    struct thread_activity_metrics mtcs = calc_activity_metrics(t);
    t->activity_metrics = mtcs;
}

static void derive_timeshare_prio_range(enum thread_activity_class cls,
                                        uint32_t *min, uint32_t *max) {
    switch (cls) {
    case THREAD_ACTIVITY_CLASS_INTERACTIVE:
        *min = THREAD_PRIO_TS_INTERACTIVE_MIN;
        *max = THREAD_PRIO_TS_INTERACTIVE_MAX;
        break;

    case THREAD_ACTIVITY_CLASS_IO_BOUND:
        *min = THREAD_PRIO_TS_IO_BOUND_MIN;
        *max = THREAD_PRIO_TS_IO_BOUND_MAX;
        break;

    case THREAD_ACTIVITY_CLASS_CPU_BOUND:
        *min = THREAD_PRIO_TS_CPU_BOUND_MIN;
        *max = THREAD_PRIO_TS_CPU_BOUND_MAX;
        break;

    case THREAD_ACTIVITY_CLASS_SLEEPY:
    default:
        *min = THREAD_PRIO_TS_SLEEPY_MIN;
        *max = THREAD_PRIO_TS_SLEEPY_MAX;
        break;
    }
}

static uint32_t compute_activity_score_pct(struct thread_activity_metrics *m) {
    uint32_t wake_norm = m->wake_freq > WAKE_FREQ_MAX ? 100 : m->wake_freq * 5;

    uint32_t interactive_pct = (wake_norm * (100u - m->block_ratio)) / 100u;

    uint32_t cpu_factor = 100u - m->run_ratio;

    uint32_t score_pct = (interactive_pct * cpu_factor) / 100u;

    uint32_t bias = score_pct / 8;

    uint32_t final_pct = score_pct + bias;

    if (final_pct > 100)
        final_pct = 100;

    return final_pct;
}

static inline int32_t jitter_for_thread(void) {
    uint32_t v = prng_next();
    uint32_t jit = THREAD_REINSERT_THRESHOLD >> 2;
    int32_t j = (int32_t) (v % (2 * jit + 1)) - (int32_t) jit;
    return j;
}

static int get_class_multiplier(enum thread_activity_class class) {
    int class_mul;
    switch (class) {
    case THREAD_ACTIVITY_CLASS_INTERACTIVE: SET_MUL(THREAD_MUL_INTERACTIVE);
    case THREAD_ACTIVITY_CLASS_IO_BOUND: SET_MUL(THREAD_MUL_IO_BOUND);
    case THREAD_ACTIVITY_CLASS_CPU_BOUND: SET_MUL(THREAD_MUL_CPU_BOUND);
    case THREAD_ACTIVITY_CLASS_SLEEPY:
    default: SET_MUL(0); /* Unclassified thread or sleepy thread */
    }
    return class_mul;
}

static inline void clamp_thread_delta(struct thread *t) {
    const int32_t max = THREAD_DELTA_MAX;
    if (t->dynamic_delta > max)
        t->dynamic_delta = max;

    if (t->dynamic_delta < -max)
        t->dynamic_delta = -max;
}

void thread_apply_wake_boost(struct thread *t) {
    if (prio_type_of(t->perceived_priority) == THREAD_PRIO_TYPE_RT)
        return;

    uint32_t score_pct = compute_activity_score_pct(&t->activity_metrics);
    int32_t mul = get_class_multiplier(t->activity_class);

    int64_t delta_change64 = score_pct * THREAD_DELTA_UNIT * mul / 100;

    delta_change64 += jitter_for_thread();

    int64_t new_delta = (int64_t) t->dynamic_delta + delta_change64;
    CLAMP(new_delta, -THREAD_DELTA_MAX, THREAD_DELTA_MAX);

    t->dynamic_delta = (int32_t) new_delta;

    thread_update_effective_priority(t);
}

static int32_t compute_cpu_penalty(struct thread *t, int32_t base_penalty) {
    struct thread_activity_metrics *m = &t->activity_metrics;
    uint32_t run_scale = m->run_ratio;
    uint32_t wake_scale = m->wake_freq * 2;
    if (wake_scale > 100)
        wake_scale = 100;

    int32_t penalty = base_penalty * run_scale / 100;
    penalty -= base_penalty * wake_scale / 200;
    if (penalty < 1)
        penalty = 1;

    return penalty;
}

void thread_apply_cpu_penalty(struct thread *t) {
    thread_calculate_activity_data(t); /* Give it another update */

    /* Apply the penalty */
    if (t->activity_class == THREAD_ACTIVITY_CLASS_CPU_BOUND) {
        /* Small penalty */
        int32_t penalty = compute_cpu_penalty(t, THREAD_PENALTY_CPU_RUN);
        int64_t scaled_delta = penalty * t->weight / MAX(t->weight, 1ULL);
        t->dynamic_delta -= scaled_delta;
        clamp_thread_delta(t);
    }

    thread_update_effective_priority(t);
}

static int64_t base_weight_of(struct thread *t) {
    struct thread_activity_metrics *m = &t->activity_metrics;

    int64_t w = 10240;

    w += m->wake_freq * 100;
    w += (100 - m->run_ratio) * 50;

    w += t->dynamic_delta / 100000;

    if (w < 1)
        w = 1;

    return w;
}

void thread_update_effective_priority(struct thread *t) {
    uint32_t min, max;
    derive_timeshare_prio_range(t->activity_class, &min, &max);

    int64_t eff = ((int64_t) min + (int64_t) max) / 2 + t->dynamic_delta;
    CLAMP(eff, min, max);

    t->priority_score = (thread_prio_t) eff;
    t->tree_node.data = t->priority_score;
    t->weight = base_weight_of(t);
}

static uint64_t compute_period(struct scheduler *s) {
    uint64_t load = s->thread_count;
    uint64_t period = BASE_PERIOD_MS + (load * 2); /* Linear growth */
    CLAMP(period, MIN_PERIOD_MS, MAX_PERIOD_MS);
    return period;
}

static void allocate_slices(struct scheduler *s, uint64_t now_ms) {
    s->period_ms = compute_period(s);
    s->period_start_ms = now_ms;

    uint64_t total_weight = 0;
    struct rbt_node *node;
    rbt_for_each(node, &s->thread_rbt) {
        struct thread *t = thread_from_rbt_node(node);
        total_weight += t->weight;
    }

    if (total_weight == 0)
        total_weight = 1;

    s->total_weight_fp = total_weight;

    rbt_for_each(node, &s->thread_rbt) {
        struct thread *t = thread_from_rbt_node(node);

        uint64_t budget_ms = (s->period_ms * t->weight) / total_weight;
        if (budget_ms < MIN_SLICE_MS)
            budget_ms = MIN_SLICE_MS;

        uint64_t slice_ms = MIN_SLICE_MS;

        if (budget_ms > MAX_SLICE_MS) {
            slice_ms = MAX_SLICE_MS;
        } else if (budget_ms > MIN_SLICE_MS) {
            slice_ms = budget_ms;
        }

        t->timeslice_duration_ms = slice_ms;
        t->timeslices_remaining = (budget_ms + slice_ms - 1) / slice_ms;
        t->completed_period = s->current_period - 1;
    }
}

static void scheduler_update_thread_weights(struct scheduler *s) {
    struct rbt_node *node;
    rbt_for_each(node, &s->thread_rbt) {
        struct thread *t = rbt_entry(node, struct thread, tree_node);

        /* This will recalculate activity data
         * and update the effective priority */
        thread_apply_cpu_penalty(t);
    }
}

void scheduler_period_start(struct scheduler *s, uint64_t now_ms) {
    s->current_period++;

    scheduler_update_thread_weights(s);

    allocate_slices(s, now_ms);

    s->period_enabled = true;
}
