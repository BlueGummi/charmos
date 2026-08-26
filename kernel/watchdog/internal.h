#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <time/time.h>
#include <watchdog.h>

bool watchdog_count_heartbeats_at(const struct watchdog_buckets *buckets,
                                  size_t window_buckets, time_ms_t now,
                                  time_ms_t bucket_interval_ms,
                                  size_t expected_per_bucket,
                                  size_t *out_heartbeats, size_t *out_expected,
                                  fx32_32_t *out_score);
