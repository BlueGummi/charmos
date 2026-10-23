/* @title: Tests */
#pragma once
#include <cmdline.h>
#include <compiler/core.h>
#include <compiler/diagnostic.h>
#include <compiler/wrapper.h>
#include <console/printf.h>
#include <err.h>
#include <fs/detect.h>
#include <log.h>
#include <math/fixed.h>
#include <math/units.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <scaled_param.h>
#include <stdbool.h>
#include <stringify.h>
#include <sync/lock_general.h>
#include <test/test_api_internal.h>

typedef void (*test_fn_t)(void);
struct test_context;
struct test_fleet;
struct thread;
struct thread_create_params;

#define TEST_MAX_HELPERS 8

struct test_alloc_node {
    struct test_alloc_node *next;
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
    TEST_SKIP_INSUFFICIENT_CORES, /* fewer online cores than .min_cores */
    TEST_SKIP_UNSUPPORTED_FS,     /* mounted root is not .required_fs */
};

enum test_state : uint8_t {
    TEST_STATE_DISABLED = 0,
    TEST_STATE_ENABLED = 1,
    TEST_STATE_SENTINEL = 0xFF,
};

struct test_group {
    const char *name;
    const char *fname;
    uint32_t line;
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

/*
 * The idea with (line|msg|stmt)_hint is that crash codes
 * get reused in various different places, and it's sometimes not enough
 * to merely check that the code matches, because it could still
 * be tripping the wrong assertion.
 *
 * So, we have these hints instead to fuzzy match and note/warn
 * when a hint does(n't) match. The expected crash code matching
 * will pass regardless, and hints just give a bit more data.
 *
 * msg_hint fuzzy matches against the message, if any, and
 * stmt_hint fuzzy matches against the statement, if any, e.g.
 *
 * enforce(my_var) would have the 'stmt' of my_var
 */
struct test_death {
    enum crash_code expect;
    uint32_t line_hint;
    const char *msg_hint;
    const char *stmt_hint;
};

struct test {
    const char *name;
    const char *fname;
    uint32_t line;

    struct test_verdict (*func)(struct test_context *ctx);

    const struct test_group *group;
    enum test_tier tier;

    size_t msg_cap;
    enum test_flags flags;

    time_ms_t duration_ms;

    /* A lot of these have to be full bools for the cmdline parse */
    struct test_death *death;
    bool print_logs;         /* Prints logs *as* they are logged */
    enum test_state enabled; /* If this is set to TEST_SENTINEL,
                              * it means we default */

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

    struct scaled_param intensity_desc;

    uint64_t seed; /* Can be passed in through cmdline, NOTE:
                    * if a seed is set through the cmdline, and
                    * run_times > 1, this seed will be used in every
                    * run during run_times */

    size_t run_times;

    /* TODO: typed core-count strong int */
    size_t min_cores;
    sz_mib_t min_ram_mib;
    enum fs_type required_fs;

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
#define TEST_FAIL_MSG_MAX 512

struct test_context {
    struct log_site *site;
    struct log_handle handle;

    uint32_t soft_fails;

    fx32_32_t intensity;  /* Raw [0, 1] fixed-point intensity */
    size_t intensity_val; /* Pre-computed scaled integer based on SCALE */
    time_ms_t duration_ms;

    uint64_t seed;

    /* Workers put their data into here */
    char fail_msg[TEST_FAIL_MSG_MAX];

    /* Allocated by the runner */
    struct test_fleet *fleet;

    /* Framework-owned allocations, freed at test teardown */
    struct test_alloc_node *allocs;

    /* Runner-managed auxiliary threads, joined at test teardown */
    struct thread *helpers[TEST_MAX_HELPERS];
    size_t helper_count;
};

void *test_alloc(struct test_context *ctx, size_t size) cw_alloc(2);

/* int *x = test_new(x); */
#define TEST_NEW_CHECK(p)                                                      \
    static_assert(                                                             \
        __builtin_types_compatible_p(__typeof_unqual__(p), typeof(&*(p))),     \
        "test_new: target must be a pointer lvalue, not an array");            \
    static_assert(                                                             \
        !cc_generic((p), void * : 1, const void * : 1, volatile void * : 1,    \
                    const volatile void * : 1, default : 0),                   \
        "test_new: void pointer target, use test_alloc() with a size")

#define test_new(p)                                                            \
    ({                                                                         \
        TEST_NEW_CHECK(p);                                                     \
        (VNEW_PTR(p)) test_alloc(ctx, sizeof(*(p)));                           \
    })

struct thread *test_spawn_internal(struct test_context *ctx,
                                   void (*entry)(void *),
                                   const struct thread_create_params *params,
                                   const char *name);

#define test_spawn(ctx, name, entry, ...)                                      \
    test_spawn_internal((ctx), (entry),                                        \
                        &(struct thread_create_params)                         \
                            thread_params_with_defaults(__VA_ARGS__),          \
                        PP_UNPAREN(name))

cpu_id_t test_core(size_t index);

bool test_context_teardown(struct test_context *ctx);

struct test_verdict {
    enum test_result result;
    enum test_skip_reason skip_reason;
    const char *msg; /* optional, e.g. the failed assertion  */
};

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

#define TEST_GROUP_NONE test_group_orphan_parent
#define TEST_GROUP(name) &(__test_group_##name)
#define TEST_GROUP_EXTERN(name) extern struct test_group __test_group_##name

#define TEST(grp, id) __test_##grp##_##id

#define TEST_DEFINE(grp, id, ...)                                              \
    cc_wno_override_init_start static struct test_verdict                      \
    __test_fn_##grp##_##id(struct test_context *ctx);                          \
    extern struct test_group __test_group_##grp;                               \
    extern struct test __test_##grp##_##id;                                    \
    LINKER_SECTION_OBJECT(struct test, tests)                                  \
    __test_##grp##_##id = {.name = #id,                                        \
                           .fname = __RELFILE__,                               \
                           .line = __LINE__,                                   \
                           .func = __test_fn_##grp##_##id,                     \
                           .run_times = 1,                                     \
                           .group = TEST_GROUP(grp),                           \
                           .enabled = TEST_STATE_SENTINEL,                     \
                           .inject_count = 0,                                  \
                           .msg_cap = 0,                                       \
                           .seed = 0,                                          \
                           .print_logs = false,                                \
                           .tier = TEST_TIER_UNIT,                             \
                           .intensity = TEST_INTENSITY_SENTINEL,               \
                           .intensity_desc = TEST_INTENSITY_DESC_SENTINEL,     \
                           .min_cores = 0,                                     \
                           .min_ram_mib = 0,                                   \
                           .required_fs = FS_UNKNOWN,                          \
                           ##__VA_ARGS__};                                     \
    cc_wno_override_init_end                                                   \
                                                                               \
        static struct test_verdict                                             \
        __test_fn_##grp##_##id(struct test_context *ctx cc_unused)

#define TEST_GROUP_DEFINE(n, ...)                                              \
    cc_wno_override_init_start extern struct test_group __test_group_##n;      \
    LINKER_SECTION_OBJECT(struct test_group, test_groups)                      \
    __test_group_##n = {.name = #n,                                            \
                        .incremental = false,                                  \
                        .exit_on_fail = false,                                 \
                        .fname = __RELFILE__,                                  \
                        .line = __LINE__,                                      \
                        .enabled = TEST_STATE_SENTINEL,                        \
                        .smoke_enabled = TEST_STATE_SENTINEL,                  \
                        .unit_enabled = TEST_STATE_SENTINEL,                   \
                        .integration_enabled = TEST_STATE_SENTINEL,            \
                        .default_intensity = TEST_INTENSITY_SENTINEL,          \
                        .intensity_desc = TEST_INTENSITY_DESC_SENTINEL,        \
                        ##__VA_ARGS__};                                        \
    cc_wno_override_init_end

#define TEST_SUCCESS ((struct test_verdict) {.result = TEST_RESULT_OK})
#define TEST_FAIL(m)                                                           \
    ((struct test_verdict) {.result = TEST_RESULT_FAILED, .msg = (m)})
#define TEST_SKIP(r)                                                           \
    ((struct test_verdict) {.result = TEST_RESULT_SKIPPED, .skip_reason = (r)})

#define TEST_ARRAY_LEN(a) ct_array_size(a)

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
    if (pmm_get_usable_ram() < MIB(8)) {                                       \
        test_info("RAM too low for test to continue!\n");                      \
        return TEST_SKIP(TEST_SKIP_RAM_LOW);                                   \
    }

void tests_run(void);
CMDLINE_EXTERN(test_root);

extern struct test_globals test_global;
extern const char *large_test_string;
extern struct test_group test_group_orphan_parent;

static inline size_t test_current_message_count(void) {
    return log_site_message_count(test_global.current_test->site);
}

STRINGIFY_DECLARE_ENUM(test_tier) {
    switch (stringify_in) {
    case TEST_TIER_SMOKE: return "smoke";
    case TEST_TIER_UNIT: return "unit";
    case TEST_TIER_INTEGRATION: return "integration";
    default: cc_unreachable();
    }
}

STRINGIFY_DECLARE(test_tier_to_str_color, enum test_tier) {
    switch (stringify_in) {
    case TEST_TIER_SMOKE: return ANSI_GRAY "smoke";
    case TEST_TIER_UNIT: return ANSI_MAGENTA "unit";
    case TEST_TIER_INTEGRATION: return ANSI_YELLOW "integration";
    default: cc_unreachable();
    }
}

STRINGIFY_DECLARE_ENUM(test_result) {
    switch (stringify_in) {
    case TEST_RESULT_OK: return ANSI_BLUE "ok" ANSI_RESET;
    case TEST_RESULT_FAILED: return ANSI_RED "failed" ANSI_RESET;
    case TEST_RESULT_SKIPPED: return ANSI_GRAY "skipped" ANSI_RESET;
    default: cc_unreachable();
    }
}

STRINGIFY_DECLARE(test_result_to_str_plain, enum test_result) {
    switch (stringify_in) {
    case TEST_RESULT_OK: return "ok";
    case TEST_RESULT_FAILED: return "failed";
    case TEST_RESULT_SKIPPED: return "skipped";
    default: return "unknown";
    }
}

STRINGIFY_DECLARE_ENUM(test_skip_reason) {
    switch (stringify_in) {
    case TEST_SKIP_NONE: return "none";
    case TEST_SKIP_RAM_LOW: return "RAM low";
    case TEST_SKIP_UPSTREAM_FAILED: return "upstream failed";
    case TEST_SKIP_DISABLED: return "disabled";
    case TEST_SKIP_INSUFFICIENT_CORES: return "insufficient cores";
    case TEST_SKIP_UNSUPPORTED_FS: return "unsupported fs";
    default: cc_unreachable();
    }
}
