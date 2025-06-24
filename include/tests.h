#pragma once
#include <console/printf.h>
#include <stdbool.h>

typedef void (*test_fn_t)(void);

struct kernel_test {
    const char *name;
    test_fn_t func;

    /* TODO: fancy state machine with enum? */
    bool should_fail;
    bool success;
    bool skipped;

    uint64_t message_count;
    char **messages;
} __attribute__((aligned(64)));

#define REGISTER_TEST(name, should_fail)                                       \
    static void name(void);                                                    \
    static struct kernel_test __test_##name                                    \
        __attribute__((section(".kernel_tests"), used)) = {                    \
            #name, name, should_fail, false, false, 0, NULL};                  \
    static void name(void)

#define SET_SUCCESS(name) __test_##name.success = true

#define SET_FAIL(name) __test_##name.success = false

#define SET_SKIP(name) __test_##name.skipped = true

#define ADD_MESSAGE(name, msg)                                                 \
    do {                                                                       \
        if (__test_##name.messages == NULL)                                    \
            __test_##name.messages = kmalloc(sizeof(char *));                  \
        else                                                                   \
            __test_##name.messages =                                           \
                krealloc(__test_##name.messages,                               \
                         sizeof(char *) * __test_##name.message_count);        \
        __test_##name.messages[__test_##name.message_count] = msg;             \
        __test_##name.message_count++;                                         \
    } while (0)

#define test_assert(x)                                                         \
    do {                                                                       \
        if (!(x)) {                                                            \
            k_printf(" assert \"%s\" failed at %s:%d ", #x, __FILE__,          \
                     __LINE__);                                                \
            return;                                                            \
        }                                                                      \
    } while (0)

void tests_run(void);
extern const char *large_test_string;
