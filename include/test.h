/* @title: Tests */
#pragma once
#include <compiler.h>
#include <console/printf.h>
#include <errno.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <stdbool.h>

typedef void (*test_fn_t)(void);
struct test_context;

enum test_tier {
    TEST_TIER_SMOKE,
    TEST_TIER_UNIT,
    TEST_TIER_INTEGRATION,
    TEST_TIER_MAX,
};

enum test_group_flags {
    TEST_GROUP_FLAG_NONE = 0,
    TEST_GROUP_FLAG_DEFAULT = 1, /* orphans get adopted by this group */
};

enum test_result {
    TEST_OK,
    TEST_FAILED,
    TEST_SKIPPED,
};

enum test_flags {
    TEST_FLAG_NONE = 0,
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
    void (*setup)(struct test_context *);
    void (*teardown)(struct test_context *);

    fx32_32_t default_intensity;

    /* [tier][num] */
    struct test **tests[TEST_TIER_MAX];
    size_t num_tests[TEST_TIER_MAX];
};

struct test {
    const char *name;

    struct test_verdict (*func)(struct test_context *ctx);

    struct test_group *group; /* if orphan, group with default */
    enum test_tier tier;

    enum test_flags flags;
    size_t msg_cap; /* per-test message array sizing */

    uint64_t message_count;
    char **messages;

    size_t inject_count;
    struct inject_site *inject[]; /* sites the runner arms for this test */
};

struct test_message {
    char *msg;
    void (*print)(struct test_message *);
};

struct test_context {
    struct test_message *messages;
    size_t message_count, message_cap;
    uint32_t soft_fails;
    uint64_t seed;
    fx32_32_t intensity; /* [0, 1] */
    time_t duration_ms;
    struct inject_site **injection_sites;
    size_t inject_count;
};

struct test_verdict {
    enum test_result result;
    enum test_skip_reason skip_reason;
    const char *msg; /* optional, e.g. the failed assertion  */
    void (*print)(const struct test_verdict *); /* optional richer render */
};

#define TEST_DECLARE(id, ...)                                                  \
    static struct test_verdict id(struct test_context *ctx);                   \
    LINKER_SECTION_OBJECT(struct test, tests)                                  \
    __test_##id = {.name = #id, .func = id, __VA_ARGS__};                      \
    static struct test_verdict id(struct test_context *ctx)

#define TEST_SUCCESS ((struct test_verdict) {.result = TEST_OK})
#define TEST_FAIL(m) ((struct test_verdict) {.result = TEST_FAILED, .msg = (m)})
#define TEST_SKIP(r)                                                           \
    ((struct test_verdict) {.result = TEST_SKIPPED, .skip_reason = (r)})

/* Messages live on the current test's context (global), so callbacks and worker
 * threads that lack a ctx handle can still record them, same as before. */
#define ADD_MESSAGE(m)                                                         \
    do {                                                                       \
        current_test->messages = krealloc(current_test->messages,              \
                                          sizeof(struct test_message) *        \
                                              ++current_test->message_count);  \
        current_test->messages[current_test->message_count - 1] =              \
            (struct test_message) {.msg = (m), .print = NULL};                 \
    } while (0)

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

#define FAIL_IF_FATAL(op) TEST_ASSERT(!ERR_IS_FATAL(op))

#define ABORT_IF_RAM_LOW()                                                     \
    if (pmm_get_usable_ram() < 1024 * 1024 * 8) {                              \
        ADD_MESSAGE("RAM too low for test to continue!\n");                    \
        return TEST_SKIP(TEST_SKIP_RAM_LOW);                                   \
    }

void tests_run(void);
extern const char *large_test_string;
extern struct test_context *current_test;
