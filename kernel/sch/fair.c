#include <asm.h>
#include <boot/stage.h>
#include <int/idt.h>
#include <mp/mp.h>
#include <registry.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <tests.h>

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
    return THREAD_ACTIVITY_CLASS_UNKNOWN;
}

void thread_classify_activity(struct thread *t) {
    t->activity_class = classify_activity(t->activity_metrics);
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

        uint64_t bucket_run = THREAD_ACTIVITY_BUCKET_DURATION -
                              b->block_duration - b->sleep_duration;

        if ((int64_t) bucket_run < 0)
            bucket_run = 0;

        total_duration += THREAD_ACTIVITY_BUCKET_DURATION;
        total_block += b->block_duration;
        total_sleep += b->sleep_duration;
        total_run += bucket_run;
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

static const int class_weight_adjust[] = {
    [THREAD_ACTIVITY_CLASS_CPU_BOUND] = -100,   /* Lower priority a bit */
    [THREAD_ACTIVITY_CLASS_IO_BOUND] = +50,     /* Small boost */
    [THREAD_ACTIVITY_CLASS_INTERACTIVE] = +200, /* Bigger boost */
    [THREAD_ACTIVITY_CLASS_SLEEPY] = 0,         /* Neutral */
    [THREAD_ACTIVITY_CLASS_UNKNOWN] = 0,
};

#define THREAD_DELTA_UNIT 0x00010000u /*/ 2^16 small increment */

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
