#pragma once

typedef void (*test_fn_t)(void);

struct kernel_test {
    const char *name;
    test_fn_t func;
} __attribute__((aligned(8)));

#define REGISTER_TEST(name)                                                    \
    static void name(void);                                                    \
    static struct kernel_test __test_##name                                    \
        __attribute__((section(".kernel_tests"), used)) = {#name, name};       \
    static void name(void)

#define assert(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            k_printf("Assertion failed: %s at %s:%d\n", #x, __FILE__,          \
                     __LINE__);                                                \
            k_panic("Test failure");                                           \
        }                                                                      \
    } while (0)

void tests_run(void);
