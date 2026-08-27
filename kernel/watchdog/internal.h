#pragma once
#include <console/crash.h>
#include <stdbool.h>
#include <stddef.h>
#include <time/time.h>
#include <watchdog.h>

#define watchdog_panic(fmt, ...)                                               \
    do {                                                                       \
        char _wd_msg[CRASH_MSG_MAX];                                           \
        snprintf(_wd_msg, sizeof(_wd_msg), fmt, ##__VA_ARGS__);                \
        crash(&(struct crash_context) {                                        \
            .source = CRASH_SOURCE_NMI_WATCHDOG,                               \
            .formats = CRASH_FMT_DEFAULT,                                      \
            .file = __FILE__,                                                  \
            .line = __LINE__,                                                  \
            .func = __func__,                                                  \
            .msg = _wd_msg,                                                    \
        });                                                                    \
    } while (0)

bool watchdog_count_heartbeats_at(const struct watchdog_buckets *buckets,
                                  size_t window_buckets, time_ms_t now,
                                  time_ms_t bucket_interval_ms,
                                  size_t expected_per_bucket,
                                  size_t *out_heartbeats, size_t *out_expected,
                                  fx32_32_t *out_score);
