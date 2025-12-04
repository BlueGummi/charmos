#include <sch/sched.h>
#include <sch/thread.h>
#include <string.h>

#include "internal.h"

static inline bool thread_wake_is_from_block(uint8_t wake_reason) {
    return wake_reason == THREAD_WAKE_REASON_BLOCKING_IO ||
           wake_reason == THREAD_WAKE_REASON_BLOCKING_MANUAL;
}

static inline bool thread_wake_is_from_sleep(uint8_t wake_reason) {
    return wake_reason == THREAD_WAKE_REASON_SLEEP_TIMEOUT ||
           wake_reason == THREAD_WAKE_REASON_SLEEP_MANUAL;
}

static const char *thread_prio_class_str(enum thread_prio_class c) {
    switch (c) {
    case THREAD_PRIO_CLASS_URGENT: return "URGENT";
    case THREAD_PRIO_CLASS_RT: return "RT";
    case THREAD_PRIO_CLASS_TIMESHARE: return "TS";
    case THREAD_PRIO_CLASS_BACKGROUND: return "BG";
    default: return "?";
    }
}

static const char *reason_str(uint8_t reason) {
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

static void print_ringbuffer(const struct thread *t, bool wake_reasons,
                             const char *label, struct thread_event_reason *buf,
                             size_t head) {
    k_printf("    %s: [\n", label);
    for (size_t i = 0; i < THREAD_EVENT_RINGBUFFER_CAPACITY; i++) {
        struct thread_event_reason *e = &buf[i];
        if (e->reason == THREAD_ASSOCIATED_REASON_NONE)
            continue;

        k_printf("        { reason: %s, ts: %lld, cycle: %llu",
                 reason_str(e->reason), (long long) e->timestamp,
                 (unsigned long long) e->cycle);

        if (e->associated_reason.reason != THREAD_ASSOCIATED_REASON_NONE) {
            if (!wake_reasons)
                k_printf(
                    ", assoc: { reason: %s, ts: %lld, cycle: %llu }",
                    reason_str(t->activity_data
                                   ->wake_reasons[e->associated_reason.reason]
                                   .reason),
                    t->activity_data->wake_reasons[e->associated_reason.reason]
                        .timestamp,
                    (unsigned long long) e->associated_reason.cycle);
        }

        if (i == head % THREAD_EVENT_RINGBUFFER_CAPACITY)
            k_printf(" <-- head");

        k_printf(" },\n");
    }
    k_printf("    ],\n");
}

void thread_print(const struct thread *t) {
    k_printf("Thread %s {\n", t->name);
    k_printf("    id: %llu,\n", (unsigned long long) t->id);
    k_printf("    state: %s,\n", thread_state_str(atomic_load(&t->state)));
    k_printf("    core: %lld,\n", (long long) t->curr_core);
    k_printf("    stack_size: %zu,\n", t->stack_size);

    /* priorities */
    k_printf("    base_prio: %s,\n", thread_prio_class_str(t->base_prio_class));
    k_printf("    perceived_prio: %s,\n",
             thread_prio_class_str(t->perceived_prio_class));
    k_printf("    effective_priority: %llu,\n",
             (unsigned long long) t->effective_priority);
    k_printf("    activity_score: %u, dynamic_delta: %d, weight: %llu,\n",
             t->activity_score, t->dynamic_delta,
             (unsigned long long) t->weight);
    k_printf("    saved_weight: %u\n", t->saved_weight);
    k_printf("    saved_prio_class: %s\n",
             thread_prio_class_str(t->saved_class));
    k_printf("    boost_count: %u\n", t->boost_count);

    k_printf("    activity_class: %s,\n",
             thread_activity_class_str(t->activity_class));

    /* time / slice info */
    k_printf("    run: %llu ms / budget: %llu ms,\n",
             (unsigned long long) t->period_runtime_raw_ms,
             (unsigned long long) t->budget_time_raw_ms);
    k_printf("    timeslice_length: %llu ms,\n",
             (unsigned long long) t->timeslice_length_raw_ms);
    k_printf("    completed_period: %llu,\n",
             (unsigned long long) t->completed_period);
    k_printf("    virtual_runtime: %llu / virtual_budget: %llu,\n",
             (unsigned long long) t->virtual_period_runtime,
             (unsigned long long) t->virtual_budget);

    /* metrics overview */
    k_printf("    activity_metrics: { run_ratio: %llu, block_ratio: %llu, "
             "sleep_ratio: %llu, wake_freq: %llu },\n",
             (unsigned long long) t->activity_metrics.run_ratio,
             (unsigned long long) t->activity_metrics.block_ratio,
             (unsigned long long) t->activity_metrics.sleep_ratio,
             (unsigned long long) t->activity_metrics.wake_freq);

    /* profiling */
    k_printf("    context_switches: %zu,\n", t->context_switches);
    k_printf("    preemptions: %zu,\n", t->preemptions);
    k_printf("    wakes: %zu, blocks: %zu, sleeps: %zu,\n", t->total_wake_count,
             t->total_block_count, t->total_sleep_count);
    k_printf("    apcs: %zu,\n", t->total_apcs_ran);
    k_printf("    creation_time: %lld ms,\n", (long long) t->creation_time_ms);

    /* APC state */
    k_printf("    executing_apc: %s,\n", t->executing_apc ? "true" : "false");
    k_printf("    special_apc_disable: %u, kernel_apc_disable: %u,\n",
             t->special_apc_disable, t->kernel_apc_disable);
    k_printf("    recent_event: %s,\n", apc_event_str(t->recent_event));

    /* activity ringbuffers */
    if (t->activity_data) {
        print_ringbuffer(t, /* wake_reasons = */ true, "wake_reasons",
                         t->activity_data->wake_reasons,
                         t->activity_data->wake_reasons_head);
        print_ringbuffer(t, /* wake_reasons = */ false, "block_reasons",
                         t->activity_data->block_reasons,
                         t->activity_data->block_reasons_head);
        print_ringbuffer(t, /* wake_reasons = */ false, "sleep_reasons",
                         t->activity_data->sleep_reasons,
                         t->activity_data->sleep_reasons_head);
    }

    k_printf("}\n");
}

static struct thread_event_reason *
most_recent(struct thread_event_reason *reasons, size_t head) {
    size_t past_head = head - 1;
    return &reasons[past_head % THREAD_EVENT_RINGBUFFER_CAPACITY];
}

static struct thread_event_reason *
wake_reason_associated_reason(struct thread_activity_data *data,
                              struct thread_event_reason *wake) {
    if (thread_wake_is_from_block(wake->reason)) {
        return &data->block_reasons[wake->associated_reason.reason %
                                    THREAD_EVENT_RINGBUFFER_CAPACITY];
    } else if (thread_wake_is_from_sleep(wake->reason)) {
        return &data->sleep_reasons[wake->associated_reason.reason %
                                    THREAD_EVENT_RINGBUFFER_CAPACITY];
    }
    return NULL;
}

static bool thread_event_reason_is_valid(struct thread_activity_data *data,
                                         struct thread_event_reason *reason) {
    struct thread_event_reason *assoc =
        wake_reason_associated_reason(data, reason);
    return assoc->cycle == reason->associated_reason.cycle;
}

static bool is_block(uint8_t reason) {
    return reason == THREAD_BLOCK_REASON_IO ||
           reason == THREAD_BLOCK_REASON_MANUAL;
}

static bool is_sleep(uint8_t reason) {
    return reason == THREAD_SLEEP_REASON_MANUAL;
}

static size_t get_bucket_index(time_t timestamp_ms) {
    return (timestamp_ms / THREAD_ACTIVITY_BUCKET_DURATION) %
           THREAD_ACTIVITY_BUCKET_COUNT;
}

static void clear_bucket(struct thread_activity_bucket *b) {
    b->block_count = 0;
    b->sleep_count = 0;
    b->wake_count = 0;
    b->block_duration = 0;
    b->sleep_duration = 0;
}

static void clear_bucket_set_cycle(struct thread_activity_bucket *b,
                                   uint64_t cycle) {
    clear_bucket(b);
    b->cycle = cycle;
}

static void advance_to_next_bucket(struct thread_activity_stats *stats,
                                   size_t steps, time_t now) {
    for (size_t i = 1; i <= steps && i <= THREAD_ACTIVITY_BUCKET_COUNT; i++) {
        size_t next_bucket = stats->current_bucket + i;
        size_t index = next_bucket % THREAD_ACTIVITY_BUCKET_COUNT;

        struct thread_activity_bucket *b = &stats->buckets[index];

        if (b->cycle != stats->current_cycle)
            clear_bucket_set_cycle(b, stats->current_cycle);
    }

    size_t new_bucket = stats->current_bucket + steps;
    stats->current_bucket = new_bucket % THREAD_ACTIVITY_BUCKET_COUNT;

    if (new_bucket > stats->current_bucket)
        stats->current_cycle++;

    stats->last_update_ms = now - (now % THREAD_ACTIVITY_BUCKET_DURATION);
}

static void advance_buckets_to_time(struct thread_activity_stats *stats,
                                    time_t ts) {
    if (ts <= stats->last_update_ms)
        return;

    time_t elapsed = ts - stats->last_update_ms;

    if (elapsed >= TOTAL_BUCKET_DURATION) {
        /* Jumped past everything, reset it */
        stats->current_cycle++;
        stats->current_bucket = get_bucket_index(ts);

        for (size_t i = 0; i < THREAD_ACTIVITY_BUCKET_COUNT; i++)
            clear_bucket_set_cycle(&stats->buckets[i], stats->current_cycle);

        stats->last_update_ms = ts - (ts % THREAD_ACTIVITY_BUCKET_DURATION);
        return;
    }

    size_t steps = elapsed / THREAD_ACTIVITY_BUCKET_DURATION;
    if (steps)
        advance_to_next_bucket(stats, steps, ts);
}

static void clear_event_slot(struct thread_event_reason *slot) {
    slot->associated_reason.reason = THREAD_ASSOCIATED_REASON_NONE;
    slot->associated_reason.cycle = 0;
    slot->reason = THREAD_EVENT_REASON_NONE;
    slot->timestamp = 0;
}

struct thread_event_reason *
thread_add_event_reason(struct thread_event_reason *ring, size_t *head,
                        uint8_t reason, uint64_t time,
                        struct thread_activity_stats *stats) {

    struct thread_event_reason *slot =
        &ring[*head % THREAD_EVENT_RINGBUFFER_CAPACITY];

    if (slot->timestamp != 0)
        slot->cycle++;

    clear_event_slot(slot);

    slot->reason = reason;
    slot->timestamp = time;
    *head = *head + 1;

    if (is_block(reason) || is_sleep(reason)) {
        advance_buckets_to_time(stats, time);
        size_t i = get_bucket_index(time);
        struct thread_activity_bucket *b = &stats->buckets[i];
        if (is_block(reason))
            b->block_count++;
        else
            b->sleep_count++;
    }

    return slot;
}

static inline void link_wake_reason(struct thread_event_reason *target_reason,
                                    struct thread_event_reason *this_reason,
                                    size_t target_link, size_t this_link) {
    struct thread_event_association *target = &target_reason->associated_reason;
    struct thread_event_association *asso = &this_reason->associated_reason;

    target->reason = this_link % THREAD_EVENT_RINGBUFFER_CAPACITY;
    asso->reason = target_link % THREAD_EVENT_RINGBUFFER_CAPACITY;

    target->cycle = this_reason->cycle;
    asso->cycle = target_reason->cycle;
}

static void update_bucket_data(struct thread_event_reason *wake,
                               struct thread_activity_bucket *bucket,
                               uint64_t overlap, uint32_t current_cycle) {
    if (bucket->cycle != current_cycle)
        clear_bucket_set_cycle(bucket, current_cycle);

    if (thread_wake_is_from_block(wake->reason)) {
        bucket->block_duration += overlap;
    } else if (thread_wake_is_from_sleep(wake->reason)) {
        bucket->sleep_duration += overlap;
    }
}

static inline uint64_t find_overlap(time_t effective_start,
                                    time_t effective_end) {
    uint64_t diff = effective_end - effective_start;
    return effective_end > effective_start ? diff : 0;
}

static void update_bucket(struct thread_activity_stats *stats,
                          struct thread_event_reason *wake, time_t start,
                          time_t end) {
    time_t bucket_start = start - (start % THREAD_ACTIVITY_BUCKET_DURATION);
    uint32_t current_cycle = stats->current_cycle;

    size_t max_buckets = THREAD_ACTIVITY_BUCKET_COUNT;
    size_t buckets_updated = 0;

    while (bucket_start < end && buckets_updated < max_buckets) {
        time_t bucket_end = bucket_start + THREAD_ACTIVITY_BUCKET_DURATION;
        size_t bucket_index = get_bucket_index(bucket_start);

        time_t effective_start = start > bucket_start ? start : bucket_start;
        time_t effective_end = end < bucket_end ? end : bucket_end;
        uint64_t overlap = find_overlap(effective_start, effective_end);

        struct thread_activity_bucket *bucket = &stats->buckets[bucket_index];

        update_bucket_data(wake, bucket, overlap, current_cycle);

        bucket_start += THREAD_ACTIVITY_BUCKET_DURATION;
        buckets_updated++;
    }
}

void thread_update_activity_stats(struct thread *t, uint64_t time) {
    struct thread_activity_stats *stats = t->activity_stats;
    struct thread_activity_data *data = t->activity_data;

    time_t now = time;

    /* Advance to next bucket if a new time window has happened */
    time_t elapsed = now - stats->last_update_ms;
    size_t steps = elapsed / THREAD_ACTIVITY_BUCKET_DURATION;

    if (steps > 0)
        advance_to_next_bucket(stats, steps, now);

    /* Gather wake event associated data */
    size_t wake_head = data->wake_reasons_head;
    size_t last = stats->last_wake_index;

    for (size_t i = last; i < wake_head; i++) {
        size_t idx = i % THREAD_EVENT_RINGBUFFER_CAPACITY;
        struct thread_event_reason *wake = &data->wake_reasons[idx];

        if (wake->associated_reason.reason == THREAD_ASSOCIATED_REASON_NONE)
            continue;

        struct thread_event_reason *start_evt =
            wake_reason_associated_reason(data, wake);

        bool start_evt_is_valid = thread_event_reason_is_valid(data, wake);
        if (!start_evt || !start_evt_is_valid)
            continue;

        time_t start = start_evt->timestamp;
        time_t end = wake->timestamp;

        if (start > end)
            start = end;

        update_bucket(stats, wake, start, end);
    }

    stats->last_wake_index = wake_head;
}

void thread_add_wake_reason(struct thread *t, uint8_t reason) {
    struct thread_activity_data *d = t->activity_data;
    time_t now = time_get_ms();

    struct thread_event_reason *curr = thread_add_event_reason(
        d->wake_reasons, &d->wake_reasons_head, reason, now, t->activity_stats);

    advance_buckets_to_time(t->activity_stats, now);
    size_t wbi = get_bucket_index(now);
    t->activity_stats->buckets[wbi].wake_count++;

    size_t this_past_head = d->wake_reasons_head - 1;
    struct thread_event_reason *past = NULL;
    size_t past_head = 0;

    if (thread_wake_is_from_block(reason)) {
        past_head = d->block_reasons_head - 1;
        past = most_recent(d->block_reasons, d->block_reasons_head);
    } else if (thread_wake_is_from_sleep(reason)) {
        past_head = d->sleep_reasons_head - 1;
        past = most_recent(d->sleep_reasons, d->sleep_reasons_head);
    } else {
        curr->associated_reason.reason = THREAD_ASSOCIATED_REASON_NONE;
    }

    if (past)
        link_wake_reason(past, curr, past_head, this_past_head);

    thread_update_activity_stats(t, now);

    t->total_wake_count++;
}

void thread_update_runtime_buckets(struct thread *thread, uint64_t time) {
    uint64_t now = time;

    /* Which seconds does this delta span? */
    uint64_t start_sec = thread->run_start_time / 1000;
    uint64_t end_sec = now / 1000;

    for (uint64_t sec = start_sec; sec <= end_sec; ++sec) {
        uint64_t bucket_index = sec % THREAD_EVENT_RINGBUFFER_CAPACITY;

        struct thread_runtime_bucket *bucket =
            &thread->activity_stats->rt_buckets[bucket_index];

        /* Reset it if it's for a different second */
        if (bucket->wall_clock_sec != sec) {
            bucket->wall_clock_sec = sec;
            bucket->run_time_ms = 0;
        }

        /* How much of delta belongs to this second? */
        uint64_t slice_start =
            (sec == start_sec) ? thread->run_start_time : sec * 1000;
        uint64_t slice_end = (sec == end_sec) ? now : (sec + 1) * 1000;

        bucket->run_time_ms += slice_end - slice_start;
    }

    uint64_t new_ms = time - thread->run_start_time;

    thread->period_runtime_raw_ms += new_ms;

    uint64_t mult = thread->activity_score == 0 ? 1 : thread->activity_score;

    thread->virtual_period_runtime += new_ms * mult;
}

void thread_add_block_reason(struct thread *t, uint8_t reason) {
    struct thread_activity_data *d = t->activity_data;
    t->total_block_count++;
    thread_add_event_reason(d->block_reasons, &d->block_reasons_head, reason,
                            time_get_ms(), t->activity_stats);
}

void thread_add_sleep_reason(struct thread *t, uint8_t reason) {
    struct thread_activity_data *d = t->activity_data;
    t->total_sleep_count++;
    thread_add_event_reason(d->sleep_reasons, &d->sleep_reasons_head, reason,
                            time_get_ms(), t->activity_stats);
}

static bool set_state_and_update_reason(
    struct thread *t, uint8_t reason, enum thread_state state,
    void (*callback)(struct thread *, uint8_t), void *wake_src,
    bool already_locked, enum thread_wait_type type) {
    /* do not preempt us. this is raised to HIGH because it is sometimes
     * called from HIGH and we can't "raise from HIGH to DISPATCH" */
    enum irql irql;

    if (!already_locked)
        irql = thread_acquire(t);
    else
        kassert(spinlock_held(&t->lock));

    /* wake_matched can only be altered from within this locked section.
     *
     * we first check at the start if we are blocking and the wake has
     * matched so that in that case we do not block and can exit our
     * block loop in wait_for_wake_match */
    bool ok = false;
    if (state != THREAD_STATE_READY) {
        if (atomic_load_explicit(&t->wake_matched, memory_order_acquire) &&
            t->wake_token == t->wait_token) {
            ok = true;
            goto out;
        }
    }

    if (state == THREAD_STATE_READY) {
        atomic_store_explicit(&t->wake_src, wake_src, memory_order_release);
        if (wake_src == t->expected_wake_src) {
            t->wake_token = t->wait_token;
            atomic_store_explicit(&t->wake_matched, true, memory_order_release);
        }
    } else {
        atomic_store_explicit(&t->wait_type, type, memory_order_release);
        t->last_action_reason = reason;
        t->last_action = state;
        t->wait_token = ++t->token_ctr;
        t->wake_token = 0;
        t->expected_wake_src = wake_src;
    }

    /* only change the state if it is NOT both RUNNING and being set to READY */
    if (!(state == THREAD_STATE_READY &&
          thread_get_state(t) == THREAD_STATE_RUNNING))
        atomic_store(&t->state, state);

    callback(t, reason);

    uint64_t time = time_get_ms();

    if (state != THREAD_STATE_READY)
        thread_update_runtime_buckets(t, time);

out:
    if (!already_locked)
        thread_release(t, irql);

    return ok;
}

void thread_wake(struct thread *t, enum thread_wake_reason r, void *wake_src) {
    set_state_and_update_reason(
        t, r, THREAD_STATE_READY, thread_add_wake_reason, wake_src,
        /*already_locked=*/false,
        /* set this to 0 because it is no-op on wake */ 0);
}

void thread_wake_locked(struct thread *t, enum thread_wake_reason r,
                        void *wake_src) {
    set_state_and_update_reason(t, r, THREAD_STATE_READY,
                                thread_add_wake_reason, wake_src,
                                /*already_locked=*/true, /* type = */ 0);
}

void thread_set_timesharing(struct thread *t) {
    t->base_prio_class = THREAD_PRIO_CLASS_TIMESHARE;
    t->perceived_prio_class = THREAD_PRIO_CLASS_TIMESHARE;
}

void thread_set_background(struct thread *t) {
    t->base_prio_class = THREAD_PRIO_CLASS_BACKGROUND;
    t->perceived_prio_class = THREAD_PRIO_CLASS_BACKGROUND;
}

bool thread_block(struct thread *t, enum thread_block_reason r,
                  enum thread_wait_type type, void *expect_wake_src) {
    return set_state_and_update_reason(t, r, THREAD_STATE_BLOCKED,
                                       thread_add_block_reason, expect_wake_src,
                                       /*already_locked=*/false, type);
}

bool thread_sleep(struct thread *t, enum thread_sleep_reason r,
                  enum thread_wait_type type, void *expect_wake_src) {
    return set_state_and_update_reason(t, r, THREAD_STATE_SLEEPING,
                                       thread_add_sleep_reason, expect_wake_src,
                                       /*already_locked=*/false, type);
}

void thread_wait_for_wake_match() {
    scheduler_yield();

    struct thread *curr = scheduler_get_current_thread();
    if (curr->last_action != THREAD_STATE_BLOCKED &&
        curr->last_action != THREAD_STATE_SLEEPING)
        return;

    /* we can safely avoid checking the token in the loop because that will
     * become set if wake_matched is set... */
    while (!atomic_load_explicit(&curr->wake_matched, memory_order_acquire)) {
        /* if this returns true, it is because the wake_matched flag was set
         * by another thread waking it up. in this case, we should simply exit
         * because we had a scenario where another thread woke us up (acquiring
         * our lock), which sets the wake_matched flag and then releases the
         * lock, which we then see afterwards */
        if (curr->last_action != THREAD_STATE_BLOCKED &&
            curr->last_action != THREAD_STATE_SLEEPING)
            k_panic("uh oh\n");

        if (curr->last_action == THREAD_STATE_BLOCKED) {
            if (thread_block(curr, curr->last_action_reason, curr->wait_type,
                             curr->expected_wake_src))
                break;
        } else {
            if (thread_sleep(curr, curr->last_action_reason, curr->wait_type,
                             curr->expected_wake_src))
                break;
        }

        scheduler_yield();
    }
    thread_clear_wake_data(curr);
}
