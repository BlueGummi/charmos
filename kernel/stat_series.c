#include <kassert.h>
#include <mem/alloc.h>
#include <stat_series.h>
#include <sync/lock_chk_assert.h>
#include <sync/lock_general.h>
#include <time/time.h>

void stat_series_init(struct stat_series *s, struct stat_bucket *buckets,
                      uint32_t nbuckets, time_us_t bucket_us,
                      stat_series_callback bucket_reset, void *private) {
    memset(buckets, 0, nbuckets * sizeof(struct stat_bucket));
    s->buckets = buckets;
    s->bucket_reset = bucket_reset;
    s->nbuckets = nbuckets;
    s->bucket_us = bucket_us;
    atomic_init(&s->current, 0);
    atomic_init(&s->last_update_us, time_get_us());
    s->private = private;
    spinlock_init(&s->lock);

    for (size_t i = 0; i < nbuckets; i++)
        s->buckets[i].parent = s;
}

struct stat_series *stat_series_create(uint32_t nbuckets, time_us_t bucket_us,
                                       stat_series_callback bucket_reset,
                                       void *private) {
    struct stat_bucket *buckets =
        valloc(sizeof(struct stat_bucket) * nbuckets, ALLOC_ZERO);
    if (!buckets)
        return NULL;

    struct stat_series *series =
        valloc(sizeof(struct stat_series), ALLOC_ZERO);
    if (!series) {
        vfree(buckets);
        return NULL;
    }

    stat_series_init(series, buckets, nbuckets, bucket_us, bucket_reset,
                     private);
    return series;
}

void stat_series_reset(struct stat_series *s) {
    enum irql irql = spin_lock(&s->lock);

    struct stat_bucket *bucket;

    stat_series_for_each(bucket, s) {
        atomic_store(&bucket->count, 0);
        atomic_store(&bucket->sum, 0);
        s->bucket_reset(bucket);
    }

    spin_unlock(&s->lock, irql);
}

static void stat_series_advance_locked(struct stat_series *s, time_us_t now_us)
    TSA_MUST_HOLD(&s->lock) {
    /* re-evaluate based on locked state (canonical update) */
    size_t delta = now_us - atomic_load_rlx(&s->last_update_us);
    uint32_t steps = delta / s->bucket_us;
    if (steps == 0)
        return;

    if (steps > s->nbuckets)
        steps = s->nbuckets;

    size_t series_current = atomic_load_rlx(&s->current);

    for (uint32_t i = 0; i < steps; i++) {
        /* update canonical non-atomic s->current while holding lock */
        size_t current = (series_current + 1) % s->nbuckets;
        struct stat_bucket *bucket = &s->buckets[current];

        atomic_store(&bucket->count, 0);
        atomic_store(&bucket->sum, 0);
        s->bucket_reset(bucket);
        atomic_store_release(&s->current, current);
    }

    /* publish last_update_us atomically for lockless readers/writers */
    atomic_store(&s->last_update_us,
                 atomic_load_rlx(&s->last_update_us) + steps * s->bucket_us);
}

void stat_series_advance(struct stat_series *s, time_us_t now_us) {
    /* Fast-path: check without taking lock */
    time_us_t last = atomic_load(&s->last_update_us);
    size_t delta = now_us - last;
    uint32_t steps = delta / s->bucket_us;
    if (steps == 0)
        return; /* no rotation required, avoid locking */

    /* Take lock and re-check/recompute using the protected fields */
    enum irql irql = spin_lock(&s->lock);
    stat_series_advance_locked(s, now_us);
    spin_unlock(&s->lock, irql);
}

void stat_series_record(struct stat_series *s, size_t value,
                        stat_series_callback callback) {
    time_us_t now_us = time_get_us();

    /* attempt to advance if needed */
    stat_series_advance(s, now_us);

    /* read canonical current published by advance (atomic load) */
    uint32_t cur = atomic_load(&s->current); /* acquire ordering */
    struct stat_bucket *b = &s->buckets[cur];

    /* hot path: only atomic RMWs on bucket */
    atomic_inc(&b->count);
    atomic_fetch_add(&b->sum, value);
    if (callback)
        callback(b);
}
