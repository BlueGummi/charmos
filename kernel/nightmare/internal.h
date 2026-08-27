#pragma once
#include <console/crash.h>
#include <nightmare/nightmare.h>
#include <stdatomic.h>
#include <sync/completion.h>
#include <thread/queue.h>
#include <time/timer.h>

#define nightmare_panic(fmt, ...)                                              \
    do {                                                                       \
        char _nm_msg[CRASH_MSG_MAX];                                           \
        snprintf(_nm_msg, sizeof(_nm_msg), fmt, ##__VA_ARGS__);                \
        crash(&(struct crash_context) {                                        \
            .source = CRASH_SOURCE_NIGHTMARE,                                  \
            .formats = CRASH_FMT_DEFAULT,                                      \
            .file = __FILE__,                                                  \
            .line = __LINE__,                                                  \
            .func = __func__,                                                  \
            .msg = _nm_msg,                                                    \
        });                                                                    \
    } while (0)

enum nightmare_on_stall : uint8_t {
    NIGHTMARE_ON_STALL_REPORT = 0,
    NIGHTMARE_ON_STALL_SNAPSHOT,
    NIGHTMARE_ON_STALL_TERMINAL,
};

struct nightmare_cmdline_config {
    const char *selector;
    fx32_32_t intensity;
    uint64_t seed;
    bool seed_present;
    enum nightmare_seed_mode seed_mode;
    time_ms_t duration_ms;
    time_ms_t drain_grace_ms;
    time_ms_t stat_interval_ms;
    enum nightmare_on_stall on_stall;
    uint64_t boot_index;
    const char *campaign_id;
    struct cmdline_list perturb;
    bool perturb_present;
};

struct nightmare_runtime {
    struct nightmare_ctx ctx;
    _Atomic enum nightmare_stop stop;
    atomic_bool quiesce_requested;
    atomic_size_t parked_count;
    atomic_size_t finding_count;
    atomic_bool terminal;
    struct nightmare_worker *workers;
    struct thread *heartbeat;
    struct completion start;
    struct timer soft_timer;
    struct timer hard_timer;
    time_ms_t started_ms;
    time_ms_t stat_interval_ms;
    const char *campaign_id;
    uint64_t boot_index;
    char caps[256];
};

extern struct nightmare_runtime nightmare_runtime;

void nightmare_publish_stop(enum nightmare_stop reason);
void nightmare_cmdline_get(struct nightmare_cmdline_config *config);
void nightmare_thread_main(void *arg);
void nightmare_heartbeat_main(void *arg);
const char *nightmare_result_string(enum nightmare_result result);
const char *nightmare_skip_string(enum nightmare_skip_reason reason);
const char *nightmare_seed_policy_string(enum nightmare_seed_policy policy);
const char *nightmare_seed_mode_string(enum nightmare_seed_mode mode);
