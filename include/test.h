/* @title: Tests */
#pragma once
#include <cmdline.h>
#include <compiler.h>
#include <console/printf.h>
#include <errno.h>
#include <log.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <stdbool.h>

typedef void (*test_fn_t)(void);
struct test_context;

enum test_cmdline_entry {
    TEST_CMDLINE_ENTRY_ENABLED,
    TEST_CMDLINE_ENTRY_MAX,
};

enum test_tier {
    TEST_TIER_SMOKE,
    TEST_TIER_UNIT,
    TEST_TIER_INTEGRATION,
    TEST_TIER_MAX,
};

enum test_options {
    TEST_OPTION_NONE = 0, /* These are passed from cmdline */
};

/* These are immutable properties of the group */
enum test_group_flags {
    TEST_GROUP_FLAG_NONE = 0,
    TEST_GROUP_FLAG_DEFAULT = 1, /* orphans get adopted by this group */
};

enum test_result {
    TEST_RESULT_OK,
    TEST_RESULT_FAILED,
    TEST_RESULT_SKIPPED,
    TEST_RESULT_MAX,
};

enum test_flags {
    TEST_FLAG_NONE = 0,

    /* Test flags will be structured in what they *enable* or honor/respect */
    TEST_FLAG_HONORS_INTENSITY = 1,
};

enum test_skip_reason {
    TEST_SKIP_NONE,
    TEST_SKIP_RAM_LOW,
    TEST_SKIP_UPSTREAM_FAILED, /* a prior tier in this group
                                  failed */
    TEST_SKIP_DISABLED,        /* test.x=off */
};

struct test_group {
    const char *name;
    const enum test_group_flags flags;
    struct cmdline_entry *ent;
    struct test_verdict (*setup)(struct test_context *);
    struct test_verdict (*teardown)(struct test_context *);

    fx32_32_t default_intensity;

    /* These all have to be bool for cmdline QOL */
    bool enabled;      /* Does anything run at all? */
    bool incremental;  /* First run smoke, then unit, then integration */
    bool exit_on_fail; /* Different from incremental: exits after one failure,
                        * whereas incremental still completes the tier */

    /* [tier][num] */
    struct test **tests[TEST_TIER_MAX];
    size_t num_tests[TEST_TIER_MAX];
};

struct test {
    const char *name;

    struct test_verdict (*func)(struct test_context *ctx);

    const struct test_group *group;
    enum test_tier tier;

    size_t msg_cap;
    enum test_flags flags;

    time_t duration_ms;

    /* A lot of these have to be full bools for the cmdline parse */
    bool print_logs; /* Prints logs *as* they are logged */
    bool enabled;
    bool keep_going; /* This basically means that with run_times,
                      * if it fails once, don't bother, and keep going
                      * until it runs run_times, however, it is overridden
                      * by exit_on_fail from the test_group */

    fx32_32_t intensity; /* also [0, 1]. the point of this one is that
                          * it can be set by the command line, and the
                          * context reads and passes this into the test.
                          *
                          * the reason for this indirection is that the
                          * struct test_context that tests get might be
                          * changed by other subsystems that may not
                          * necessarily keep the same .intensity as the
                          * boot time command line option/default */

    uint64_t seed; /* Can be passed in through cmdline, NOTE:
                    * if a seed is set through the cmdline, and
                    * run_times > 1, this seed will be used in every
                    * run during run_times */

    size_t run_times;

    size_t inject_count;
    struct cmdline_entry *base_entry;
    struct cmdline_entry *cmdline_entries[TEST_CMDLINE_ENTRY_MAX];
    struct inject_site *inject[];
};
#define TEST_SEED_SENTINEL 0

/* The reason this exists is because the actual structures in struct
 * test are not meant to be accessed from inside the test's func,
 * and this is the way the test interacts with the variables.
 *
 * This might make one think "why not just interact with struct test?",
 * and the answer is that struct test_context gives us extensibility,
 * and allows a test to get run numerous different times with varied
 * contexts, seeds, etc. without having to fiddle around with struct test */
struct test_context {
    struct log_site *site;
    struct log_handle handle;

    uint32_t soft_fails;
    fx32_32_t intensity; /* [0, 1] */
    time_t duration_ms;

    uint64_t seed;
};

struct test_verdict {
    enum test_result result;
    enum test_skip_reason skip_reason;
    const char *msg; /* optional, e.g. the failed assertion  */
};

struct test_globals {
    size_t results[TEST_TIER_MAX][TEST_RESULT_MAX];
    size_t results_agg[TEST_RESULT_MAX];
    struct test_context *current_test;
    time_t total_time;
};

#define TEST(id) __test_##id
#define TEST_DECLARE(id, ...)                                                  \
    static struct test_verdict id(struct test_context *ctx);                   \
    extern struct test __test_##id;                                            \
    CMDLINE_ENTRY_DECLARE(test_##id, .name = #id,                              \
                          .flags = CMDLINE_ENTRY_SYMBOLIC);                    \
    CMDLINE_ENTRY_DECLARE_TYPED(                                               \
        test_##id##_enabled, __test_##id.enabled, .name = "enabled",           \
        .parent = CMDLINE_ENTRY(test_##id), .private = &__test_##id);          \
    CMDLINE_ENTRY_DECLARE_TYPED(                                               \
        test_##id##_intensity, __test_##id.intensity, .name = "intensity",     \
        .parent = CMDLINE_ENTRY(test_##id), .private = &__test_##id);          \
    CMDLINE_ENTRY_DECLARE_TYPED(                                               \
        test_##id##_run_times, __test_##id.run_times, .name = "run_times",     \
        .parent = CMDLINE_ENTRY(test_##id), .private = &__test_##id);          \
    CMDLINE_ENTRY_DECLARE_TYPED(                                               \
        test_##id##_seed, __test_##id.seed, .name = "seed",                    \
        .parent = CMDLINE_ENTRY(test_##id), .private = &__test_##id);          \
    CMDLINE_ENTRY_DECLARE_TYPED(                                               \
        test_##id##_duration_ms, __test_##id.duration_ms,                      \
        .name = "duration_ms", .parent = CMDLINE_ENTRY(test_##id),             \
        .private = &__test_##id);                                              \
    CMDLINE_ENTRY_DECLARE_TYPED(                                               \
        test_##id##_msg_cap, __test_##id.msg_cap, .name = "msg_cap",           \
        .parent = CMDLINE_ENTRY(test_##id), .private = &__test_##id);          \
    CMDLINE_ENTRY_DECLARE_TYPED(                                               \
        test_##id##_keep_going, __test_##id.keep_going, .name = "keep_going",  \
        .parent = CMDLINE_ENTRY(test_##id), .private = &__test_##id);          \
    CMDLINE_ENTRY_DECLARE_TYPED(                                               \
        test_##id##_print_logs, __test_##id.print_logs, .name = "print_logs",  \
        .parent = CMDLINE_ENTRY(test_##id), .private = &__test_##id);          \
    LINKER_SECTION_OBJECT(struct test, tests)                                  \
    __test_##id = {.name = #id,                                                \
                   .func = id,                                                 \
                   .run_times = 1,                                             \
                   .group = &test_group_orphan_parent,                         \
                   .enabled = true,                                            \
                   .base_entry = CMDLINE_ENTRY(test_##id),                     \
                   .inject_count = 0,                                          \
                   .msg_cap = 0,                                               \
                   .seed = 0,                                                  \
                   .print_logs = false,                                        \
                   .tier = TEST_TIER_UNIT,                                     \
                   .cmdline_entries[TEST_CMDLINE_ENTRY_ENABLED] =              \
                       CMDLINE_ENTRY(test_##id##_enabled),                     \
                   __VA_ARGS__};                                               \
                                                                               \
    static struct test_verdict id(struct test_context *ctx __unused)

#define TEST_CMDLINE_ENTRY_DECLARE(id, var, n)                                 \
    CMDLINE_ENTRY_DECLARE_TYPED(test_##id##_##n, var, .name = #n,              \
                                .parent = CMDLINE_ENTRY(test_##id),            \
                                .private = &__test_##id)

#define TEST_GROUP_DECLARE(n, ...)                                             \
    extern struct test_group __test_group_##n;                                 \
    CMDLINE_ENTRY_DECLARE(test_group_##n, .name = #n,                          \
                          .parent = CMDLINE_ENTRY(test_root),                  \
                          .flags = CMDLINE_ENTRY_SYMBOLIC)                     \
    CMDLINE_ENTRY_DECLARE_TYPED(test_group_##n##_enabled,                      \
                                __test_group_##n.enabled, .name = "enabled",   \
                                .parent = CMDLINE_ENTRY(test_group_##n),       \
                                .private = &__test_group_##n);                 \
    CMDLINE_ENTRY_DECLARE_TYPED(                                               \
        test_group_##n##_incremental, __test_group_##n.incremental,            \
        .name = "incremental", .parent = CMDLINE_ENTRY(test_group_##n),        \
        .private = &__test_group_##n);                                         \
    CMDLINE_ENTRY_DECLARE_TYPED(                                               \
        test_group_##n##_exit_on_fail, __test_group_##n.exit_on_fail,          \
        .name = "exit_on_fail", .parent = CMDLINE_ENTRY(test_group_##n),       \
        .private = &__test_group_##n);                                         \
    LINKER_SECTION_OBJECT(struct test_group, test_groups)                      \
    __test_group_##n = {.name = #n,                                            \
                        .incremental = false,                                  \
                        .exit_on_fail = false,                                 \
                        .enabled = true,                                       \
                        __VA_ARGS__}

#define TEST_GROUP(name) __test_group_##name

#define TEST_SUCCESS ((struct test_verdict) {.result = TEST_RESULT_OK})
#define TEST_FAIL(m)                                                           \
    ((struct test_verdict) {.result = TEST_RESULT_FAILED, .msg = (m)})
#define TEST_SKIP(r)                                                           \
    ((struct test_verdict) {.result = TEST_RESULT_SKIPPED, .skip_reason = (r)})

#define TEST_ASSERT(x)                                                         \
    do {                                                                       \
        if (!(x)) {                                                            \
            printf(" assert \"%s\" failed at %s:%d ", #x, __FILE__, __LINE__); \
            return TEST_FAIL(#x);                                              \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_VOID(x)                                                    \
    do {                                                                       \
        if (!(x)) {                                                            \
            printf(" assert \"%s\" failed at %s:%d ", #x, __FILE__, __LINE__); \
            return;                                                            \
        }                                                                      \
    } while (0)

#define test_log(lvl, fmt, ...)                                                \
    log(test_global.current_test->site, &test_global.current_test->handle,     \
        lvl, fmt, ##__VA_ARGS__)

#define test_err(fmt, ...) test_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define test_warn(fmt, ...) test_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define test_info(fmt, ...) test_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define test_debug(fmt, ...) test_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define test_trace(fmt, ...) test_log(LOG_TRACE, fmt, ##__VA_ARGS__)

#define FAIL_IF_FATAL(op) TEST_ASSERT(!ERR_IS_FATAL(op))

#define ABORT_IF_RAM_LOW()                                                     \
    if (pmm_get_usable_ram() < 1024 * 1024 * 8) {                              \
        test_info("RAM too low for test to continue!\n");                      \
        return TEST_SKIP(TEST_SKIP_RAM_LOW);                                   \
    }

void tests_run(void);
void tests_hook_boot();
CMDLINE_ENTRY_DEFINE(test_root);

extern struct test_globals test_global;
extern const char *large_test_string;
extern struct test_group test_group_orphan_parent;
extern struct cmdline_entry test_cmdline_default;

static inline size_t test_current_message_count(void) {
    return log_site_message_count(test_global.current_test->site);
}

static inline const char *test_tier_to_str(enum test_tier tier) {
    switch (tier) {
    case TEST_TIER_SMOKE: return "smoke";
    case TEST_TIER_UNIT: return "unit";
    case TEST_TIER_INTEGRATION: return "integration";
    default: kassert_unreachable();
    }
}

static inline const char *test_result_to_str(enum test_result result) {
    switch (result) {
    case TEST_RESULT_OK: return ANSI_BLUE "ok" ANSI_RESET;
    case TEST_RESULT_FAILED: return ANSI_RED "failed" ANSI_RESET;
    case TEST_RESULT_SKIPPED: return ANSI_GRAY "skipped" ANSI_RESET;
    default: kassert_unreachable();
    }
}

static inline const char *
test_skip_reason_to_str(enum test_skip_reason reason) {
    switch (reason) {
    case TEST_SKIP_NONE: return "none";
    case TEST_SKIP_RAM_LOW: return "RAM low";
    case TEST_SKIP_UPSTREAM_FAILED: return "upstream failed";
    case TEST_SKIP_DISABLED: return "disabled";
    default: kassert_unreachable();
    }
}
