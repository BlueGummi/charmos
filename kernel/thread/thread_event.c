#include <math/min_max.h>
#include <sch/sched.h>
#include <string.h>
#include <thread/apc.h>
#include <thread/thread.h>

#include "sch/internal.h"

static inline bool resume_is_from_sleep(uint8_t wake_reason) {
    return wake_reason == THREAD_WAKE_REASON_SLEEP_TIMEOUT ||
           wake_reason == THREAD_WAKE_REASON_SLEEP_MANUAL;
}

static inline bool wait_reason_is_sleep(thread_act_reason_t reason) {
    return reason == THREAD_SLEEP_REASON_MANUAL;
}

static const char *thread_prio_class_str(enum thread_prio_class c) {
    switch (c) {
    case THREAD_PRIO_CLASS_URGENT: return "URGENT";
    case THREAD_PRIO_CLASS_REALTIME: return "RT";
    case THREAD_PRIO_CLASS_TIMESHARE: return "TS";
    case THREAD_PRIO_CLASS_BACKGROUND: return "BG";
    default: return "?";
    }
}

static const char *reason_str(thread_act_reason_t reason) {
    switch (reason) {
    case THREAD_WAKE_REASON_BLOCKING_IO: return "WAKE_IO";
    case THREAD_WAKE_REASON_BLOCKING_MANUAL: return "WAKE_MANUAL";
    case THREAD_WAKE_REASON_SLEEP_TIMEOUT: return "WAKE_TIMEOUT";
    case THREAD_WAKE_REASON_SLEEP_MANUAL: return "WAKE_SLEEP_MANUAL";
    case THREAD_BLOCK_REASON_IO: return "BLOCK_IO";
    case THREAD_BLOCK_REASON_MANUAL: return "BLOCK_MANUAL";
    case THREAD_SLEEP_REASON_MANUAL: return "SLEEP_MANUAL";
    case THREAD_EVENT_REASON_NONE: return "NONE";
    default: return "?";
    }
}

static void print_events(const struct thread *t) {
    const struct thread_activity_data *d = &t->activity_data;
    size_t newest = (d->head - 1) & THREAD_EVENT_RINGBUFFER_MASK;
    bool has_newest = d->events[newest].wake_reason != THREAD_EVENT_REASON_NONE;

    if (d->active_wait_reason != THREAD_EVENT_REASON_NONE) {
        printf("    active_wait: { reason: %s, start_ms: %u },\n",
               reason_str(d->active_wait_reason), d->wait_start_ms);
    }

    printf("    events: [\n");
    for (size_t i = 0; i < THREAD_EVENT_RINGBUFFER_CAPACITY; i++) {
        const struct thread_event_reason *e = &d->events[i];
        if (e->wake_reason == THREAD_EVENT_REASON_NONE &&
            e->wait_reason == THREAD_EVENT_REASON_NONE)
            continue;

        printf("        { wait: %s, wake: %s, dur: %u ms",
               reason_str(e->wait_reason), reason_str(e->wake_reason),
               e->duration_ms);

        if (has_newest && i == newest)
            printf(" <-- newest");

        printf(" },\n");
    }
    printf("    ],\n");
}

void thread_print(const struct thread *t) {
    printf("Thread %s {\n", t->name);
    printf("    id: %llu,\n", (unsigned long long) t->id);
    printf("    state: %s,\n", thread_state_str(atomic_load(&t->state)));
    printf("    core: %lld,\n", (long long) t->curr_core);
    printf("    stack_size: %zu,\n", t->stack_size);

    /* priorities */
    printf("    base_prio: %s,\n", thread_prio_class_str(t->base_prio_class));
    printf("    perceived_prio: %s,\n",
           thread_prio_class_str(t->perceived_prio_class));
    printf("    activity_score: %u, dynamic_delta: %d, weight: %llu,\n",
           t->activity_score, t->dynamic_delta, (unsigned long long) t->weight);

#ifdef DEBUG_THREAD
    printf("    boost_count: %u\n", t->boost_count);
#endif

    printf("    activity_class: %s,\n",
           thread_activity_class_str(t->activity_class));

    /* time / slice info */
    printf("    run: %llu ms / budget: %llu ms,\n",
           (unsigned long long) t->period_runtime_raw_ms,
           (unsigned long long) t->budget_time_raw_ms);
    printf("    timeslice_length: %llu ms,\n",
           (unsigned long long) t->timeslice_length_raw_ms);
    printf("    completed_period: %llu,\n",
           (unsigned long long) t->completed_period);
    printf("    virtual_runtime: %llu / virtual_budget: %llu,\n",
           (unsigned long long) t->virtual_period_runtime,
           (unsigned long long) t->virtual_budget);

    /* metrics overview */
    printf("    activity_metrics: { run_ratio: %llu, block_ratio: %llu, "
           "sleep_ratio: %llu, wake_freq: %llu },\n",
           (unsigned long long) t->activity_metrics.run_ratio,
           (unsigned long long) t->activity_metrics.block_ratio,
           (unsigned long long) t->activity_metrics.sleep_ratio,
           (unsigned long long) t->activity_metrics.wake_freq);

    /* profiling */
#ifdef DEBUG_THREAD
    printf("    context_switches: %zu,\n", t->context_switches);
    printf("    preemptions: %zu,\n", t->preemptions);
    printf("    wakes: %u, blocks: %u, sleeps: %u,\n", t->total_wake_count,
           t->total_block_count, t->total_sleep_count);
    printf("    apcs: %u,\n", t->total_apcs_ran);
    printf("    creation_time: %lld ms,\n", (long long) t->creation_time_ms);
#endif

    /* APC state */
    printf("    executing_apc: %s,\n",
           (atomic_load_rlx(&t->flags) & THREAD_FLAG_EXECUTING_APC) ? "true"
                                                                    : "false");
    printf("    special_apc_disable: %u, kernel_apc_disable: %u,\n",
           t->special_apc_disable, t->kernel_apc_disable);

    /* activity events ringbuffer */
    print_events(t);

    printf("}\n");
}

enum thread_bucket_span_kind {
    SPAN_RUN,
    SPAN_BLOCK,
    SPAN_SLEEP,
};

static inline void ensure_bucket_epoch(struct thread_activity_bucket *b,
                                       uint32_t epoch) {
    if (b->epoch != epoch) {
        b->epoch = epoch;
        b->run_ms = 0;
        b->block_ms = 0;
        b->sleep_ms = 0;
        b->wake_count = 0;
    }
}

static void bucket_add_span(struct thread_activity_stats *stats,
                            time_ms_t start_ms, time_ms_t end_ms,
                            enum thread_bucket_span_kind kind) {
    if (end_ms <= start_ms)
        return;

    uint64_t start = start_ms;
    uint64_t end = end_ms;

    uint64_t start_epoch = start >> THREAD_ACTIVITY_BUCKET_SHIFT;
    uint64_t end_epoch = end >> THREAD_ACTIVITY_BUCKET_SHIFT;
    uint64_t orig_start_epoch = start_epoch;

    if (end_epoch - start_epoch >= THREAD_ACTIVITY_BUCKET_COUNT)
        start_epoch = end_epoch - (THREAD_ACTIVITY_BUCKET_COUNT - 1);

    for (uint64_t ep = start_epoch; ep <= end_epoch; ++ep) {
        size_t idx = ep & THREAD_ACTIVITY_INDEX_MASK;
        struct thread_activity_bucket *b = &stats->buckets[idx];

        ensure_bucket_epoch(b, (uint32_t) ep);

        uint64_t bucket_start = ep << THREAD_ACTIVITY_BUCKET_SHIFT;
        uint64_t bucket_end = (ep + 1) << THREAD_ACTIVITY_BUCKET_SHIFT;

        uint64_t slice_start = ep == orig_start_epoch ? start : bucket_start;
        uint64_t slice_end = ep == end_epoch ? end : bucket_end;
        uint32_t overlap =
            slice_end > slice_start ? (uint32_t) (slice_end - slice_start) : 0;

        uint16_t *field;
        if (kind == SPAN_RUN) {
            field = &b->run_ms;
        } else if (kind == SPAN_BLOCK) {
            field = &b->block_ms;
        } else {
            field = &b->sleep_ms;
        }

        uint32_t sum = *field + overlap;
        *field = sum > THREAD_ACTIVITY_BUCKET_DURATION
                     ? THREAD_ACTIVITY_BUCKET_DURATION
                     : sum;
    }
}

void thread_add_wake_reason(struct thread *t, thread_act_reason_t reason) {
    struct thread_activity_data *d = &t->activity_data;
    time_ms_t now = time_get_ms();
    uint16_t clamped = 0;
    thread_act_reason_t wait_reason = d->active_wait_reason;

    if (wait_reason != THREAD_EVENT_REASON_NONE) {
        /* Explicitly truncate and cut off the high bits to save some space */
        uint32_t elapsed_32 = (uint32_t) now - d->wait_start_ms;
        CLAMP(clamped, 0, UINT16_MAX);

        time_ms_t start = now > elapsed_32 ? now - elapsed_32 : 0;
        bool is_sleep =
            wait_reason_is_sleep(wait_reason) || resume_is_from_sleep(reason);

        bucket_add_span(&t->activity_stats, start, now,
                        is_sleep ? SPAN_SLEEP : SPAN_BLOCK);

        d->active_wait_reason = THREAD_EVENT_REASON_NONE;
    }

    uint32_t now_epoch = (uint32_t) (now >> THREAD_ACTIVITY_BUCKET_SHIFT);
    size_t wbi = now_epoch & THREAD_ACTIVITY_INDEX_MASK;
    struct thread_activity_bucket *wb = &t->activity_stats.buckets[wbi];
    ensure_bucket_epoch(wb, now_epoch);
    if (wb->wake_count < UINT16_MAX)
        wb->wake_count++;

    struct thread_event_reason *slot =
        &d->events[d->head & THREAD_EVENT_RINGBUFFER_MASK];

    slot->duration_ms = clamped;
    slot->wait_reason = wait_reason;
    slot->wake_reason = reason;
    d->head++;

#ifdef DEBUG_THREAD
    t->total_wake_count++;
#endif
}

void thread_update_runtime_buckets(struct thread *thread, time_ms_t time) {
    if (time <= thread->run_start_time)
        return;

    bucket_add_span(&thread->activity_stats, thread->run_start_time, time,
                    SPAN_RUN);

    uint64_t new_ms = (uint64_t) (time - thread->run_start_time);

    thread->period_runtime_raw_ms += new_ms;

    uint64_t mult = thread->activity_score == 0 ? 1 : thread->activity_score;

    thread->virtual_period_runtime += new_ms * mult;
}

void thread_add_block_reason(struct thread *t, thread_act_reason_t reason) {
#ifdef DEBUG_THREAD
    t->total_block_count++;
#endif
    t->activity_data.wait_start_ms = (uint32_t) time_get_ms();
    t->activity_data.active_wait_reason = reason;
}

void thread_add_sleep_reason(struct thread *t, thread_act_reason_t reason) {
#ifdef DEBUG_THREAD
    t->total_sleep_count++;
#endif
    t->activity_data.wait_start_ms = (uint32_t) time_get_ms();
    t->activity_data.active_wait_reason = reason;
}

void thread_set_timesharing(struct thread *t) {
    enum irql pirql = spin_lock_high(&t->pi_lock);

    t->base_prio_class = THREAD_PRIO_CLASS_TIMESHARE;
    thread_recompute_prio_class_locked(t);

    spin_unlock(&t->pi_lock, pirql);
}

void thread_set_background(struct thread *t) {
    enum irql pirql = spin_lock_high(&t->pi_lock);

    t->base_prio_class = THREAD_PRIO_CLASS_BACKGROUND;
    thread_recompute_prio_class_locked(t);

    spin_unlock(&t->pi_lock, pirql);
}
