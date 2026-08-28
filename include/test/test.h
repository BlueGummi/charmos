/* @title: Tests */
#pragma once
#include <cmdline.h>
#include <compiler.h>
#include <console/printf.h>
#include <errno.h>
#include <log.h>
#include <math/fixed.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <scaled_param.h>
#include <stdbool.h>

typedef void (*test_fn_t)(void);
struct test_context;

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

    /* If this is enabled, TEST_FLAG_HONORS_INTENSITY is auto-enabled */
    TEST_FLAG_INHERITS_INTENSITY = 1 << 1,
};

enum test_skip_reason {
    TEST_SKIP_NONE,
    TEST_SKIP_RAM_LOW,
    TEST_SKIP_UPSTREAM_FAILED, /* a prior tier in this group
                                  failed */
    TEST_SKIP_DISABLED,        /* not the same as .enabled, just that the test
                                * won't be getting run for some reason */
};

enum test_state : uint8_t {
    TEST_STATE_DISABLED = 0,
    TEST_STATE_ENABLED = 1,
    TEST_STATE_SENTINEL = 0xFF,
};

struct test_group {
    const char *name;
    const char *fname;
    const enum test_group_flags flags;
    struct test_verdict (*setup)(struct test_context *);
    struct test_verdict (*teardown)(struct test_context *);

    fx32_32_t default_intensity;

    /* This is given to all children that TEST_FLAG_INHERITS_INTENSITY */
    struct scaled_param intensity_desc;

    enum test_state enabled;
    union {
        enum test_state tier_enabled[TEST_TIER_MAX];
        struct {
            enum test_state smoke_enabled;
            enum test_state unit_enabled;
            enum test_state integration_enabled;
        };
    };

    bool incremental;  /* First run smoke, then unit, then integration */
    bool exit_on_fail; /* Different from incremental: exits after one failure,
                        * whereas incremental still completes the tier */

    /* [tier][num] */
    struct test **tests[TEST_TIER_MAX];
    size_t num_tests[TEST_TIER_MAX];
    size_t num_tests_enabled[TEST_TIER_MAX];
};

struct test {
    const char *name;

    struct test_verdict (*func)(struct test_context *ctx);

    const struct test_group *group;
    enum test_tier tier;

    size_t msg_cap;
    enum test_flags flags;

    time_ms_t duration_ms;

    /* A lot of these have to be full bools for the cmdline parse */
    bool print_logs;         /* Prints logs *as* they are logged */
    enum test_state enabled; /* If this is set to TEST_SENTINEL,
                              * it means we default */
    bool keep_going;         /* This basically means that with run_times,
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

    struct scaled_param intensity_desc;

    uint64_t seed; /* Can be passed in through cmdline, NOTE:
                    * if a seed is set through the cmdline, and
                    * run_times > 1, this seed will be used in every
                    * run during run_times */

    size_t run_times;

    size_t inject_count;
    struct inject_site *inject[];
};

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

    fx32_32_t intensity;  /* Raw [0, 1] fixed-point intensity */
    size_t intensity_val; /* Pre-computed scaled integer based on SCALE */
    time_ms_t duration_ms;

    uint64_t seed;
};

struct test_verdict {
    enum test_result result;
    enum test_skip_reason skip_reason;
    const char *msg; /* optional, e.g. the failed assertion  */
};

#define TEST_EXIT_OK 0
#define TEST_EXIT_FAIL 1
#define TEST_INTENSITY_SENTINEL ((fx32_32_t) - 1LL)
#define TEST_INTENSITY_DEFAULT FX(0.5)

/* Intended for use with TEST_FLAG_INHERITS_INTENSITY, so that tests can still
 * set custom min, def, max outside of what they would inherit from group */
#define TEST_INTENSITY(min, def, max)                                          \
    .flags = TEST_FLAG_INHERITS_INTENSITY,                                     \
    .intensity_desc = {.min_val = (min), .def_val = (def), .max_val = (max)}
#define TEST_INTENSITY_DESC_SENTINEL                                           \
    (struct scaled_param) {                                                    \
        .min_val = SIZE_MAX, .def_val = SIZE_MAX, .max_val = SIZE_MAX          \
    }

struct test_globals {
    fx32_32_t global_intensity;
    size_t total_tests_enabled;
    size_t results[TEST_TIER_MAX][TEST_RESULT_MAX];
    size_t results_agg[TEST_RESULT_MAX];
    struct test_context *current_test;
    time_ms_t total_time;
    bool show_output;
    bool group_opt_in;
    bool test_opt_in;
    bool no_exit;
    bool no_progress;
};

#define TEST_GROUP_NONE test_group_orphan_parent

/* Goofy macros needed for the DECLARE macro */
#define __test_group_TEST_GROUP_NONE test_group_orphan_parent
#define __test_group_none test_group_orphan_parent
#define __test_group_orphan test_group_orphan_parent
#define __test_group_test_group_orphan_parent test_group_orphan_parent

#define TEST_GROUP(name) &(__test_group_##name)
#define TEST_GROUP_DEFINE(name) extern struct test_group __test_group_##name

#define TEST(id) __test_##id
#define TEST_DECLARE(grp, id, ...)                                             \
    static struct test_verdict id(struct test_context *ctx);                   \
    extern struct test_group __test_group_##grp;                               \
    extern struct test __test_##id;                                            \
    LINKER_SECTION_OBJECT(struct test, tests)                                  \
    __test_##id = {.name = #id,                                                \
                   .func = id,                                                 \
                   .run_times = 1,                                             \
                   .group = TEST_GROUP(grp),                                   \
                   .enabled = TEST_STATE_SENTINEL,                             \
                   .inject_count = 0,                                          \
                   .msg_cap = 0,                                               \
                   .seed = 0,                                                  \
                   .print_logs = false,                                        \
                   .tier = TEST_TIER_UNIT,                                     \
                   .intensity = TEST_INTENSITY_SENTINEL,                       \
                   .intensity_desc = TEST_INTENSITY_DESC_SENTINEL,             \
                   ##__VA_ARGS__};                                             \
                                                                               \
    static struct test_verdict id(struct test_context *ctx __unused)

#define TEST_DECLARE_SMOKE(grp, id, ...)                                       \
    TEST_DECLARE(grp, id, .tier = TEST_TIER_SMOKE, ##__VA_ARGS__)
#define TEST_DECLARE_UNIT(grp, id, ...)                                        \
    TEST_DECLARE(grp, id, .tier = TEST_TIER_UNIT, ##__VA_ARGS__)
#define TEST_DECLARE_INTEGRATION(grp, id, ...)                                 \
    TEST_DECLARE(grp, id, .tier = TEST_TIER_INTEGRATION, ##__VA_ARGS__)

#define TEST_GROUP_DECLARE(n, ...)                                             \
    extern struct test_group __test_group_##n;                                 \
    LINKER_SECTION_OBJECT(struct test_group, test_groups)                      \
    __test_group_##n = {.name = #n,                                            \
                        .incremental = false,                                  \
                        .exit_on_fail = false,                                 \
                        .fname = __RELFILE__,                                  \
                        .enabled = TEST_STATE_SENTINEL,                        \
                        .smoke_enabled = TEST_STATE_SENTINEL,                  \
                        .unit_enabled = TEST_STATE_SENTINEL,                   \
                        .integration_enabled = TEST_STATE_SENTINEL,            \
                        .default_intensity = TEST_INTENSITY_SENTINEL,          \
                        .intensity_desc = TEST_INTENSITY_DESC_SENTINEL,        \
                        ##__VA_ARGS__}

/* 1D piecewise-log */
#define TEST_INTENSITY_LOG(min, def, max, unit_str)                            \
    .flags = TEST_FLAG_HONORS_INTENSITY, .intensity_desc = {                   \
                                             .curve = SCALE_PIECEWISE_LOG,     \
                                             .min_val = (min),                 \
                                             .def_val = (def),                 \
                                             .max_val = (max),                 \
                                             .unit = (unit_str),               \
    }

/* Linear scaling */
#define TEST_INTENSITY_LINEAR(min, def, max, unit_str)                         \
    .flags = TEST_FLAG_HONORS_INTENSITY, .intensity_desc = {                   \
                                             .curve = SCALE_PIECEWISE_LINEAR,  \
                                             .min_val = (min),                 \
                                             .def_val = (def),                 \
                                             .max_val = (max),                 \
                                             .unit = (unit_str),               \
    }

/* Core-scaled thread counts (threads = base * core_count) */
#define TEST_INTENSITY_CORES(min_per_core, def_per_core, max_per_core,         \
                             unit_str)                                         \
    .flags = TEST_FLAG_HONORS_INTENSITY, .intensity_desc = {                   \
                                             .curve = SCALE_CORE_MULTIPLIER,   \
                                             .min_val = (min_per_core),        \
                                             .def_val = (def_per_core),        \
                                             .max_val = (max_per_core),        \
                                             .unit = (unit_str),               \
    }

/* set scale with min and max */
#define TEST_INTENSITY_CUSTOM_PRINT(scale, min, def, max, unit_str, print_fn)  \
    .flags = TEST_FLAG_HONORS_INTENSITY, .intensity_desc = {                   \
                                             .curve = scale,                   \
                                             .min_val = (min),                 \
                                             .def_val = (def),                 \
                                             .max_val = (max),                 \
                                             .unit = (unit_str),               \
                                             .custom_print = (print_fn),       \
    }

/* Entirely custom */
#define TEST_INTENSITY_CUSTOM(scale_fn, print_fn)                              \
    .flags = TEST_FLAG_HONORS_INTENSITY, .intensity_desc = {                   \
                                             .curve = SCALE_CUSTOM,            \
                                             .custom_scale = (scale_fn),       \
                                             .custom_print = (print_fn),       \
    }

#define TEST_SUCCESS ((struct test_verdict) {.result = TEST_RESULT_OK})
#define TEST_FAIL(m)                                                           \
    ((struct test_verdict) {.result = TEST_RESULT_FAILED, .msg = (m)})
#define TEST_SKIP(r)                                                           \
    ((struct test_verdict) {.result = TEST_RESULT_SKIPPED, .skip_reason = (r)})

#define TEST_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define test_log(lvl, fmt, ...)                                                \
    log(test_global.current_test->site, &test_global.current_test->handle,     \
        lvl, fmt, ##__VA_ARGS__)

#define test_err(fmt, ...) test_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define test_warn(fmt, ...) test_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define test_info(fmt, ...) test_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define test_debug(fmt, ...) test_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define test_trace(fmt, ...) test_log(LOG_TRACE, fmt, ##__VA_ARGS__)

#include <test/assert.h>

#define ABORT_IF_RAM_LOW()                                                     \
    if (pmm_get_usable_ram() < 1024 * 1024 * 8) {                              \
        test_info("RAM too low for test to continue!\n");                      \
        return TEST_SKIP(TEST_SKIP_RAM_LOW);                                   \
    }

void tests_run(void);
CMDLINE_DEFINE(test_root);

extern struct test_globals test_global;
extern const char *large_test_string;
extern struct test_group test_group_orphan_parent;

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

static inline const char *test_tier_to_str_color(enum test_tier tier) {
    switch (tier) {
    case TEST_TIER_SMOKE: return ANSI_GRAY "smoke";
    case TEST_TIER_UNIT: return ANSI_MAGENTA "unit";
    case TEST_TIER_INTEGRATION: return ANSI_YELLOW "integration";
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
