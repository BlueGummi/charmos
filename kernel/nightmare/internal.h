#pragma once
#include <atomic.h>
#include <console/crash.h>
#include <nightmare/nightmare.h>
#include <nightmare/perturb.h>
#include <stringify.h>
#include <sync/completion.h>
#include <test/conc.h>
#include <thread/queue.h>
#include <time/timer.h>
#include <watchdog.h>

#define nightmare_panic(fmt, ...)                                              \
    do {                                                                       \
        char _nm_msg[CRASH_MSG_MAX];                                           \
        snprintf(_nm_msg, sizeof(_nm_msg), fmt, ##__VA_ARGS__);                \
        crash_full(&(struct crash_context) {                                   \
            .source = CRASH_SOURCE_NIGHTMARE,                                  \
            .formats = CRASH_FMT_DEFAULT,                                      \
            .file = __FILE__,                                                  \
            .line = __LINE__,                                                  \
            .func = __func__,                                                  \
            .msg = _nm_msg,                                                    \
        });                                                                    \
    } while (0)

struct nightmare_cmdline_config {
    const char *selector;
    fx32_32_t intensity;
    uint64_t seed;
    bool seed_present;
    enum nightmare_seed_mode seed_mode;
    time_ms_t duration_ms;
    time_ms_t drain_grace_ms;
    time_ms_t stat_interval_ms;
    time_ms_t stall_threshold_ms;
    enum test_on_stall on_stall;
    uint64_t boot_index;
    const char *campaign_id;
    struct cmdline_list perturb;
    bool perturb_present;
};

#define NIGHTMARE_MAX_PERTURBERS 8

struct nightmare_runtime {
    struct nightmare_ctx ctx;

    struct test_conc conc;

    atomic_size_t finding_count;
    atomic_bool terminal;
    atomic_bool perturb_verdict_ready;
    struct nightmare_verdict perturb_verdict;
    char perturb_reason[64];
    char perturb_msg[256];
    size_t perturber_count;
    const struct test_perturb_desc *perturbers[NIGHTMARE_MAX_PERTURBERS];
    struct timer soft_timer;
    struct timer hard_timer;
    time_ms_t started_ms;
    time_ms_t stat_interval_ms;
    const char *campaign_id;
    uint64_t boot_index;
    char caps[256];
};

extern struct nightmare_runtime nightmare_runtime;
extern struct test_liveness_state nightmare_liveness;

void nightmare_publish_stop(enum test_stop reason);
void nightmare_cmdline_get(struct nightmare_cmdline_config *config);
void nightmare_thread_main(void *arg);
void nightmare_heartbeat_main(void *arg);
void nightmare_publish_perturb_verdict(struct nightmare_verdict verdict);
bool nightmare_load_perturb_verdict(struct nightmare_verdict *out);
enum nightmare_result
nightmare_result_with_findings(enum nightmare_result result, size_t findings);
struct nightmare_verdict
nightmare_verdict_for_stop(struct nightmare_verdict verdict,
                           enum test_stop stop);

STRINGIFY_DECLARE_ENUM(nightmare_result) {
    switch (stringify_in) {
    case NIGHTMARE_RESULT_OK: return "ok";
    case NIGHTMARE_RESULT_FINDING: return "finding";
    case NIGHTMARE_RESULT_FAIL: return "fail";
    case NIGHTMARE_RESULT_STALL: return "stall";
    case NIGHTMARE_RESULT_SKIP: return "skip";
    default: cc_unreachable();
    }
}

STRINGIFY_DECLARE(nightmare_skip_to_str, enum nightmare_skip_reason) {
    switch (stringify_in) {
    case NIGHTMARE_SKIP_NONE: return "none";
    case NIGHTMARE_SKIP_NOT_COMPILED: return "not_compiled";
    case NIGHTMARE_SKIP_NO_SUCH_NIGHTMARE: return "no_such_nightmare";
    case NIGHTMARE_SKIP_NEEDS_SMP: return "needs_smp";
    case NIGHTMARE_SKIP_NEEDS_PREEMPT: return "needs_preempt";
    case NIGHTMARE_SKIP_NEEDS_ASAN: return "needs_asan";
    case NIGHTMARE_SKIP_NEEDS_INJECT: return "needs_inject";
    case NIGHTMARE_SKIP_RAM_LOW: return "ram_low";
    case NIGHTMARE_SKIP_SEED_UNUSED: return "seed_unused";
    case NIGHTMARE_SKIP_SEED_MISSING: return "seed_missing";
    case NIGHTMARE_SKIP_SERVICE_MISSING: return "service_missing";
    case NIGHTMARE_SKIP_PREPARE_REFUSED: return "prepare_refused";
    default: return "unknown";
    }
}

STRINGIFY_DECLARE_ENUM(nightmare_seed_policy) {
    switch (stringify_in) {
    case NIGHTMARE_SEED_IGNORED: return "ignored";
    case NIGHTMARE_SEED_OPTIONAL: return "optional";
    case NIGHTMARE_SEED_REQUIRED: return "required";
    default: cc_unreachable();
    }
}

STRINGIFY_DECLARE_ENUM(nightmare_seed_mode) {
    switch (stringify_in) {
    case NIGHTMARE_SEED_SPLIT: return "split";
    case NIGHTMARE_SEED_SEEDFUL: return "seedful";
    case NIGHTMARE_SEED_SEEDLESS: return "seedless";
    default: cc_unreachable();
    }
}

bool nightmare_liveness_start(time_ms_t threshold_ms,
                              enum test_on_stall policy);
void nightmare_liveness_stop(void);
void nightmare_liveness_poll(void);
