#include <asm.h>
#include <boot/stage.h>
#include <crypto/prng.h>
#include <int/idt.h>
#include <mp/mp.h>
#include <registry.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <tests.h>

static uint64_t prio_base_and_ceil_from_base(enum thread_priority base);

#define THREAD_DELTA_UNIT 0x00010000u /* 2^16 small increment */
#define Q16_ONE (1u << 16)

#define THREAD_BOOST_WAKE_SMALL                                                \
    (4 * THREAD_DELTA_UNIT) /* provisional boost for wake */

#define THREAD_BOOST_WAKE_LARGE                                                \
    (12 * THREAD_DELTA_UNIT) /* stronger boost if very interactive */

#define THREAD_PENALTY_CPU_RUN                                                 \
    (3 * THREAD_DELTA_UNIT) /* penalize heavy CPU usage per run period */

#define THREAD_DELTA_MAX                                                       \
    (0x1000 * THREAD_DELTA_UNIT) /* clamp delta magnitude */

#define THREAD_REINSERT_THRESHOLD                                              \
    (8 * THREAD_DELTA_UNIT) /* only reinsert if effective priority changes >=  \
                               this */
#define THREAD_HYSTERESIS_MS 250 /* don't change class more often than this */
#define THREAD_PROV_BOOST_MS 50  /* provisional boost expiration */

/* How many delta units per Q16 score? */
#define THREAD_MUL_INTERACTIVE 24 /* Big boost */
#define THREAD_MUL_IO_BOUND 10    /* Small boost */
#define THREAD_MUL_CPU_BOUND 0    /* No boost */
#define THREAD_MUL_SLEEPY 0       /* No boost */

/* Scheduling periods */
#define MIN_PERIOD_MS 20  /* don’t go too short */
#define MAX_PERIOD_MS 300 /* don’t go too long */
#define BASE_PERIOD_MS 50 /* baseline for small loads */

/* Timeslices */
#define MIN_SLICE_MS 2  /* smallest slice granularity */
#define MAX_SLICE_MS 20 /* cap slice length to avoid hogs */

#define LIM(__min, __max)                                                      \
    min = __min;                                                               \
    max = __max;                                                               \
    break;

#define SET_MUL(__multiplier)                                                  \
    class_mul = __multiplier;                                                  \
    break;

#define CLAMP(__var, __min, __max)                                             \
    if (__var > __max)                                                         \
        __var = __max;                                                         \
    if (__var < __min)                                                         \
        __var = __min;

#define DERIVE_BASE_AND_CEIL(__prio, __min, __max)                             \
    uint64_t __ceil_full = prio_base_and_ceil_from_base(__prio);               \
    __min = __ceil_full & 0xFFFFFFFF;                                          \
    __max = __ceil_full >> 32ULL;

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

        int64_t brun = rtb->run_time_ms - b->block_duration - b->sleep_duration;
        if (brun < 0)
            brun = 0;

        total_duration += rtb->run_time_ms;
        total_block += b->block_duration;
        total_sleep += b->sleep_duration;
        total_run += brun;
        total_wake_count += b->wake_count;
    }

    if (total_duration > 0) {
        m.run_ratio = (total_run * 100) / total_duration;
        m.block_ratio = (total_block * 100) / total_duration;
        m.sleep_ratio = (total_sleep * 100) / total_duration;
    }

    m.wake_freq = total_wake_count / THREAD_ACTIVITY_BUCKET_COUNT;

    return m;
}

void thread_calculate_activity_data(struct thread *t) {
    struct thread_activity_metrics mtcs = calc_activity_metrics(t);
    t->activity_metrics = mtcs;
}

static uint64_t prio_base_and_ceil_from_base(enum thread_priority base) {
    uint32_t min, max;
    switch (base) {
    /* These two thread prios do not have any prio_t */
    case THREAD_PRIO_URGENT: return 0;
    case THREAD_PRIO_RT: return 0;

    case THREAD_PRIO_HIGH: LIM(THREAD_PRIO_HIGH_BASE, THREAD_PRIO_HIGH_CEIL);
    case THREAD_PRIO_MID: LIM(THREAD_PRIO_MID_BASE, THREAD_PRIO_MID_CEIL);
    case THREAD_PRIO_LOW:
    default: LIM(THREAD_PRIO_LOW_BASE, THREAD_PRIO_LOW_CEIL);
    }
    return (uint64_t) max << 32ULL | min;
}

thread_prio_t thread_base_prio32_from_base(enum thread_priority base,
                                           int nice) {
    uint32_t bucket_min, bucket_max;
    DERIVE_BASE_AND_CEIL(base, bucket_min, bucket_max);

    int32_t nice_offset = (nice /* -20 .. +19 */ + 20);
    uint64_t span = (uint64_t) bucket_max - bucket_min;
    uint64_t pos = (span * (uint64_t) nice_offset) / 39ULL;
    return (thread_prio_t) (bucket_min + pos);
}

static uint32_t compute_activity_score_q16(struct thread_activity_metrics *m) {
    /* m->run_ratio, block_ratio, sleep_ratio are 0..100 */
    /* m->wake_freq = wakes/sec (0..inf, but we clamp) */

    /* Normalize wake frequency to avoid 💥 numbers */
    uint32_t wake_norm = m->wake_freq > 20 ? 100 : (m->wake_freq * 5);

    /* Prefer high wake_norm and small block_ratio */
    /* interactive_pct = wake_norm * (100 - block_ratio) / 100 */
    uint32_t interactive_pct = (wake_norm * (100u - m->block_ratio)) / 100u;

    /* CPU Bound reduces interactivity
     * strong run_ratio -> reduce */
    uint32_t cpu_factor = (100u - (uint32_t) m->run_ratio);

    /* score_pct = interactive_pct * (100 - run_ratio) / 100 */
    uint32_t score_pct = (interactive_pct * cpu_factor) / 100u;

    /* Add a bit of bias to give small signals some weight */
    uint32_t bias = score_pct / 8; /* 12.5% of score is bias */
    uint32_t final_pct = score_pct + bias;
    if (final_pct > 100)
        final_pct = 100;

    /* Turn it to Q16 */
    return (final_pct * Q16_ONE) / 100u;
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
    CLAMP(t->dynamic_delta, -(int32_t) THREAD_DELTA_MAX,
          (int32_t) THREAD_DELTA_MAX);
}

void thread_apply_wake_boost(struct thread *t) {
    /* Do nothing */
    if (prio_class_of(t->perceived_priority) == THREAD_PRIO_CLASS_RT)
        return;

    uint32_t score_q16 = compute_activity_score_q16(&t->activity_metrics);

    int class_mul = get_class_multiplier(t->activity_class);

    /* Raw units */
    uint32_t raw_units = (((uint64_t) score_q16 * (uint64_t) class_mul) >> 16);

    /* Delta change to apply */
    int32_t delta_change = (int32_t) raw_units * (int32_t) THREAD_DELTA_UNIT;

    /* Small jitter to prevent massive overlaps */
    int32_t j = jitter_for_thread();
    delta_change += j;

    t->dynamic_delta += delta_change;
    clamp_thread_delta(t);

    thread_update_effective_priority(t);
}

void thread_apply_cpu_penalty(struct thread *t) {
    thread_calculate_activity_data(t); /* Give it another update */

    /* Apply the penalty */
    if (t->activity_class == THREAD_ACTIVITY_CLASS_CPU_BOUND) {
        /* Small penalty */
        t->dynamic_delta -= THREAD_PENALTY_CPU_RUN;
        clamp_thread_delta(t);
    }

    thread_update_effective_priority(t);
}

void thread_update_effective_priority(struct thread *t) {
    thread_prio_t eff = t->prio32_base + t->dynamic_delta;

    /* Clamp to bucket range */
    uint32_t min, max;
    DERIVE_BASE_AND_CEIL(t->perceived_priority, min, max);
    CLAMP(eff, min, max);

    t->cached_prio32 = eff;
    t->priority_score = eff;
    t->tree_node.data = eff;
    t->weight_fp = (uint64_t) (t->priority_score) << 16;
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
        total_weight += t->weight_fp;
    }

    if (total_weight == 0)
        total_weight = 1;

    s->total_weight_fp = total_weight;

    rbt_for_each(node, &s->thread_rbt) {
        struct thread *t = thread_from_rbt_node(node);

        uint64_t budget_ms = (s->period_ms * t->weight_fp) / total_weight;
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

        thread_update_effective_priority(t);
    }
}

void scheduler_period_start(struct scheduler *s, uint64_t now_ms) {
    s->current_period++;

    scheduler_update_thread_weights(s);

    allocate_slices(s, now_ms);

    s->period_enabled = true;
}
