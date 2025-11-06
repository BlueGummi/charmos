#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sync/spinlock.h>

/*
 * Generic time-bucketed statistics series.
 *
 * Subsystems can embed one or more struct stat_series to track
 * short-term rates (e.g., allocations/sec, frees/sec, requests/sec, etc).
 *
 * Each bucket represents a fixed-duration window
 * The current bucket advances as time passes
 */

struct stat_bucket {
    atomic_size_t count; /* count for this bucket */
    atomic_size_t sum;   /* optional aggregate */
    void *private;       /* private, per subsystem */
};
typedef size_t (*stat_series_callback)(struct stat_bucket *bucket);

struct stat_series {
    stat_series_callback bucket_reset; /* to call upon bucket reset */
    struct stat_bucket *buckets;       /* ringbuffer */
    uint32_t nbuckets;                 /* how many buckets */
    uint32_t current;                  /* current bucket idx */
    time_t bucket_us;                  /* duration for each bucket */
    time_t last_update_us;             /* last time we advanced */
    void *private;                     /* private, per subsystem */
    struct spinlock lock;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(stat_series, lock);

struct stat_series *stat_series_create(uint32_t nbuckets, time_t bucket_us,
                                       stat_series_callback bucket_reset,
                                       void *private);

void stat_series_init(struct stat_series *s, struct stat_bucket *buckets,
                      uint32_t nbuckets, time_t bucket_us,
                      stat_series_callback bucket_reset, void *private);
void stat_series_reset(struct stat_series *s);
void stat_series_record(struct stat_series *s, size_t value,
                        stat_series_callback callback);

size_t stat_series_count_sum(struct stat_series *s, uint32_t nbuckets,
                             stat_series_callback sum_callback);

size_t stat_series_value_sum(struct stat_series *s, uint32_t nbuckets,
                             stat_series_callback sum_callback);

void stat_series_advance(struct stat_series *s, time_t now_us);

#define stat_series_for_each(series, iter)                                     \
    for (uint32_t __i = 0;                                                     \
         (iter = &((series)->buckets[__i]), __i < (series)->nbuckets); __i++)

#define STAT_SERIES_DEFINE(name, n, bucket_us)                                 \
    static struct stat_bucket name##_buckets[(n)] = {0};                       \
    static struct stat_series name = {                                         \
        .buckets = name##_buckets,                                             \
        .nbuckets = (n),                                                       \
        .bucket_us = (bucket_us),                                              \
    };

#define STAT_SERIES_CUR_BUCKET(s) (&(s)->buckets[(s)->current])
