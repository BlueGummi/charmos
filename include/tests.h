#pragma once
#include <console/printf.h>
#include <errno.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <stdbool.h>

typedef void (*test_fn_t)(void);

struct kernel_test {
    const char *name;
    test_fn_t func;
    bool is_integration;

    bool should_fail;
    bool success;
    bool skipped;

    uint64_t message_count;
    char **messages;
} __attribute__((aligned(64)));

#define REGISTER_TEST(name, should_fail, is_integration)                       \
    static void name(void);                                                    \
    static struct kernel_test __test_##name                                    \
        __attribute__((section(".kernel_tests"), used)) = {                    \
            #name, name, is_integration, should_fail, false, false, 0, NULL};  \
    static void name(void)

#define SET_SUCCESS() current_test->success = true

#define SET_FAIL() current_test->success = false

#define SET_SKIP() () current_test->skipped = true

#define ADD_MESSAGE(msg)                                                       \
    do {                                                                       \
        current_test->messages =                                               \
            krealloc(current_test->messages,                                   \
                     sizeof(char *) * ++current_test->message_count);          \
        current_test->messages[current_test->message_count - 1] = msg;         \
    } while (0)

#define TEST_ASSERT(x)                                                         \
    do {                                                                       \
        if (!(x)) {                                                            \
            k_printf(" assert \"%s\" failed at %s:%d ", #x, __FILE__,          \
                     __LINE__);                                                \
            return;                                                            \
        }                                                                      \
    } while (0)

#define FAIL_IF_FATAL(op) TEST_ASSERT(!ERR_IS_FATAL(op))

#define ABORT_IF_RAM_LOW()                                                     \
    if (pmm_get_usable_ram() < 1024 * 1024 * 8) {                              \
        ADD_MESSAGE("RAM too low for test to continue!\n");                    \
        SET_SKIP();                                                            \
        return;                                                                \
    }

void tests_run(void);
extern const char *large_test_string;
extern struct kernel_test *current_test;

#define IS_INTEGRATION_TEST true
#define IS_UNIT_TEST false
#define SHOULD_FAIL true
#define SHOULD_NOT_FAIL false
