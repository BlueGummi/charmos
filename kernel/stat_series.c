#include <kassert.h>
#include <mem/alloc.h>
#include <stat_series.h>

void stat_series_init(struct stat_series *s, struct stat_bucket *buckets,
                      uint32_t nbuckets, time_t bucket_us,
                      stat_series_callback bucket_reset, void *private) {
    memset(buckets, 0, nbuckets * sizeof(struct stat_bucket));
    s->buckets = buckets;
    s->bucket_reset = bucket_reset;
    s->nbuckets = nbuckets;
    s->bucket_us = bucket_us;
    s->current = 0;
    s->last_update_us = time_get_us();
    s->private = private;
    spinlock_init(&s->lock);
}

struct stat_series *stat_series_create(uint32_t nbuckets, time_t bucket_us,
                                       stat_series_callback bucket_reset,
                                       void *private) {
    struct stat_bucket *buckets =
        kzalloc(sizeof(struct stat_bucket) * nbuckets);
    if (!buckets)
        return NULL;

    struct stat_series *series = kzalloc(sizeof(struct stat_series));
    if (!series) {
        kfree(buckets);
        return NULL;
    }

    stat_series_init(series, buckets, nbuckets, bucket_us, bucket_reset,
                     private);
    return series;
}

void stat_series_reset(struct stat_series *s) {
    enum irql irql = stat_series_lock(s);

    struct stat_bucket *bucket;

    stat_series_for_each(s, bucket) {
        atomic_store(&bucket->count, 0);
        atomic_store(&bucket->sum, 0);
        bucket->private = NULL;
        s->bucket_reset(bucket);
    }

    stat_series_unlock(s, irql);
}

void stat_series_advance_internal(struct stat_series *s, time_t now_us,
                                  bool already_locked) {
    enum irql irql;
    if (!already_locked) {
        irql = stat_series_lock(s);
    } else {
        kassert(spinlock_held(&s->lock));
    }

    size_t delta = now_us - s->last_update_us;
    uint32_t steps = delta / s->bucket_us;

    if (steps == 0)
        goto out;

    if (steps > s->nbuckets)
        steps = s->nbuckets;

    for (uint32_t i = 0; i < steps; i++) {
        s->current = (s->current + 1) % s->nbuckets;
        struct stat_bucket *bucket = &s->buckets[s->current];

        atomic_store(&bucket->count, 0);
        atomic_store(&bucket->sum, 0);
        bucket->private = NULL;
        s->bucket_reset(bucket);
    }

    s->last_update_us += steps * s->bucket_us;

out:
    if (!already_locked)
        stat_series_unlock(s, irql);
}

void stat_series_advance(struct stat_series *s, time_t now_us) {
    stat_series_advance_internal(s, now_us, /* already_locked = */ false);
}

void stat_series_record(struct stat_series *s, size_t value,
                        stat_series_callback callback) {
    enum irql irql = stat_series_lock(s);

    time_t now_us = time_get_us();
    stat_series_advance_internal(s, now_us, /* already_locked = */ true);

    struct stat_bucket *b = &s->buckets[s->current];
    atomic_fetch_add(&b->count, 1);
    atomic_fetch_add(&b->sum, value);
    callback(b);

    stat_series_unlock(s, irql);
}

size_t stat_series_count_sum(struct stat_series *s, uint32_t nbuckets,
                             stat_series_callback sum_callback) {
    enum irql irql = stat_series_lock(s);

    size_t total = 0;
    if (nbuckets > s->nbuckets)
        nbuckets = s->nbuckets;

    for (uint32_t i = 0; i < nbuckets; i++) {
        uint32_t idx = (s->current + s->nbuckets - i) % s->nbuckets;
        total += atomic_load(&s->buckets[idx].count);
        total += sum_callback(&s->buckets[idx]);
    }

    stat_series_unlock(s, irql);
    return total;
}

size_t stat_series_value_sum(struct stat_series *s, uint32_t nbuckets,
                             stat_series_callback sum_callback) {
    enum irql irql = stat_series_lock(s);
    size_t total = 0;
    if (nbuckets > s->nbuckets)
        nbuckets = s->nbuckets;

    for (uint32_t i = 0; i < nbuckets; i++) {
        uint32_t idx = (s->current + s->nbuckets - i) % s->nbuckets;
        total += atomic_load(&s->buckets[idx].sum);
        total += sum_callback(&s->buckets[idx]);
    }

    stat_series_unlock(s, irql);
    return total;
}
