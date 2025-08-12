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

enum thread_activity_class thread_classify_activity(struct thread_activity_metrics m) {
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
