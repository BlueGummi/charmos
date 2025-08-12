#include <sch/sched.h>
#include <sch/thread.h>

void thread_log_event_reasons(struct thread *t) {
    k_printf("Thread %llu event reason logs:\n", t->id);

    struct thread_activity_stats *stats = t->activity_stats;
    for (size_t i = 0; i < THREAD_ACTIVITY_BUCKET_COUNT; i++) {
        struct thread_activity_bucket *b = &stats->buckets[i];
        struct thread_runtime_bucket *bk = &t->activity_stats->rt_buckets[i];

        k_printf(
            "Bucket %llu shows: block_count %llu, sleep_count %llu, "
            "wake_count %llu, "
            "block_duration %llu, sleep_duration %llu, run duration %llu\n",
            i, b->block_count, b->sleep_count, b->wake_count, b->block_duration,
            b->sleep_duration, bk->run_time_ms);
    }
    k_printf("Thread activity metrics show block ratio %llu, sleep ratio %llu, "
             "run ratio %llu, wake freq %llu\n",
             t->activity_metrics.block_ratio, t->activity_metrics.sleep_ratio,
             t->activity_metrics.run_ratio, t->activity_metrics.wake_freq);
    k_printf("Thread activity class is %s\n",
             thread_activity_class_str(t->activity_class));
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

struct thread_event_reason *
thread_add_event_reason(struct thread_event_reason *ring, size_t *head,
                        uint8_t reason, uint64_t time) {

    size_t next_head = *head + 1;

    struct thread_event_reason *this_reason =
        &ring[*head % THREAD_EVENT_RINGBUFFER_CAPACITY];

    if (this_reason->timestamp != 0)
        this_reason->cycle++;

    this_reason->reason = reason;
    this_reason->timestamp = time;

    *head = next_head;

    return this_reason;
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

void thread_add_wake_reason(struct thread *t, uint8_t reason) {
    struct thread_activity_data *d = t->activity_data;
    struct thread_event_reason *curr = thread_add_event_reason(
        d->wake_reasons, &d->wake_reasons_head, reason, time_get_ms());

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

    thread_update_activity_stats(t, time_get_ms());
}

static inline size_t get_bucket_index(time_t timestamp_ms) {
    return (timestamp_ms / THREAD_ACTIVITY_BUCKET_DURATION) %
           THREAD_ACTIVITY_BUCKET_COUNT;
}

static inline void clear_bucket(struct thread_activity_bucket *b) {
    b->block_count = 0;
    b->sleep_count = 0;
    b->wake_count = 0;
    b->block_duration = 0;
    b->sleep_duration = 0;
}

static void advance_to_next_bucket(struct thread_activity_stats *stats,
                                   size_t steps, time_t now) {

    for (size_t i = 1; i <= steps && i <= THREAD_ACTIVITY_BUCKET_COUNT; ++i) {
        size_t next_bucket = stats->current_bucket + i;
        size_t index = next_bucket % THREAD_ACTIVITY_BUCKET_COUNT;
        clear_bucket(&stats->buckets[index]);
    }
    size_t new_bucket = stats->current_bucket + steps;
    stats->current_bucket = new_bucket % THREAD_ACTIVITY_BUCKET_COUNT;
    stats->last_update_ms = now - (now % THREAD_ACTIVITY_BUCKET_DURATION);
}

static void update_bucket_data(struct thread_event_reason *wake,
                               struct thread_activity_bucket *bucket,
                               uint64_t overlap) {
    bucket->wake_count++;
    if (thread_wake_is_from_block(wake->reason)) {
        bucket->block_count++;
        bucket->block_duration += overlap;
    } else if (thread_wake_is_from_sleep(wake->reason)) {
        bucket->sleep_count++;
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
    /* What buckets does this fall into? */
    time_t bucket_start = start - (start % THREAD_ACTIVITY_BUCKET_DURATION);
    while (bucket_start < end) {
        time_t bucket_end = bucket_start + THREAD_ACTIVITY_BUCKET_DURATION;
        size_t bucket_index = get_bucket_index(bucket_start);

        /* Find out overlap */
        time_t effective_start = start > bucket_start ? start : bucket_start;
        time_t effective_end = end < bucket_end ? end : bucket_end;

        uint64_t overlap = find_overlap(effective_start, effective_end);

        struct thread_activity_bucket *bucket = &stats->buckets[bucket_index];
        update_bucket_data(wake, bucket, overlap);

        bucket_start += THREAD_ACTIVITY_BUCKET_DURATION;
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

    for (size_t i = last; i < wake_head; ++i) {
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

        if (start > end) {
            k_panic("Potential corrupted timestamp\n");
            continue;
        }

        update_bucket(stats, wake, start, end);
    }

    stats->last_wake_index = wake_head;
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
}
