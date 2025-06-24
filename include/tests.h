#pragma once
#include <console/printf.h>
#include <stdbool.h>

typedef void (*test_fn_t)(void);

struct kernel_test {
    const char *name;
    test_fn_t func;
    bool should_fail;
    bool success;
} __attribute__((aligned(64)));

#define REGISTER_TEST(name, should_fail)                                       \
    static void name(void);                                                    \
    static struct kernel_test __test_##name __attribute__((                    \
        section(".kernel_tests"), used)) = {#name, name, should_fail, false};  \
    static void name(void)

#define SET_SUCCESS(name) __test_##name.success = true

#define SET_FAIL(name) __test_##name.success = false

#define test_assert(x)                                                         \
    do {                                                                       \
        if (!(x)) {                                                            \
            k_printf(" assert \"%s\" failed at %s:%d ", #x, __FILE__, __LINE__);    \
            return;                                                            \
        }                                                                      \
    } while (0)

void tests_run(void);
extern const char *large_test_string;
