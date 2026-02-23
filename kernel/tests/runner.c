#include <console/panic.h>
#include <console/printf.h>
#include <global.h>
#include <irq/irq.h>
#include <sleep.h>
#include <smp/core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tests.h>
#include <time.h>

#include <colors.h>

LOG_SITE_DECLARE_DEFAULT(test);
LOG_HANDLE_DECLARE_DEFAULT(test);

#define test_log(lvl, fmt, ...)                                                \
    log(LOG_SITE(test), LOG_HANDLE(test), lvl, fmt, ##__VA_ARGS__)

#define test_err(fmt, ...) test_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define test_warn(fmt, ...) test_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define test_info(fmt, ...) test_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define test_debug(fmt, ...) test_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define test_trace(fmt, ...) test_log(LOG_TRACE, fmt, ##__VA_ARGS__)

extern struct kernel_test __skernel_tests[];
extern struct kernel_test __ekernel_tests[];

/* no need to clean up allocations in these tests, we are supposed to
 * reboot/poweroff after all tests complete, and the userland should
 * not be in a state where we can boot it when running tests */

struct kernel_test *current_test = NULL;
static uint64_t pass_count = 0, skip_count = 0, fail_count = 0;
static uint64_t total_time = 0;

static void run(bool run_units, struct kernel_test *start,
                struct kernel_test *end) {
    uint64_t i = 1;
    for (struct kernel_test *t = start; t < end; t++) {
        if (t->is_integration && run_units)
            continue;

        if (!t->is_integration && !run_units)
            continue;

        current_test = t;
        k_printf("[%-4d]: ", i);
        k_printf("%s... ", t->name);

        uint64_t start_ms = time_get_ms();
        /* supa important */
        t->func();
        uint64_t end_ms = time_get_ms();

        if (t->skipped) {
            k_printf(ANSI_GRAY " skipped  " ANSI_RESET);
            skip_count++;
        } else if (t->success != t->should_fail) {
            k_printf(ANSI_GREEN " ok  " ANSI_RESET);
            pass_count++;
        } else {
            k_printf(ANSI_RED " error  " ANSI_RESET);
            fail_count++;
        }

        total_time += (end_ms - start_ms);
        k_printf("(%llu ms)\n", end_ms - start_ms);

        if (t->message_count > 0) {
            for (uint64_t i = 0; i < t->message_count; i++) {
                k_printf("        +-> ");
                k_printf(ANSI_YELLOW "%s" ANSI_RESET "\n", t->messages[i]);
            }
            k_printf("\n");
        }
        i++;
    }
}

void tests_run(void) {
#ifdef TEST_ENABLED
    struct kernel_test *start = __skernel_tests;
    struct kernel_test *end = __ekernel_tests;

    bool all_ok = true;
    char *msg = all_ok ? "all tests pass 🎉!" : "some errors occurred";
    char *color = all_ok ? ANSI_GREEN : ANSI_RED;

    uint64_t unit_test_count = 0;
    uint64_t integration_test_count = 0;
    for (struct kernel_test *t = start; t < end; t++) {
        if (!t->is_integration)
            unit_test_count++;
        else
            integration_test_count++;
    }
    uint64_t total_test_count = unit_test_count + integration_test_count;

    test_info("running %llu " ANSI_CYAN "unit" ANSI_RESET " tests...\n",
              unit_test_count);

    run(true, start, end);

    test_info("running %llu " ANSI_MAGENTA "integration" ANSI_RESET
              " tests...\n",
              integration_test_count);

    run(false, start, end);

    all_ok = fail_count == 0;
    color = all_ok ? ANSI_GREEN : ANSI_RED;
    msg = all_ok ? "all tests pass 🎉!" : "some errors occurred";
    char *fail_color = all_ok ? ANSI_GREEN : ANSI_RED;
    char *skip_color = all_ok ? ANSI_GREEN : ANSI_GRAY;

    test_info("%llu " ANSI_CYAN "total" ANSI_RESET " tests, %llu " ANSI_GREEN
              "passed" ANSI_RESET ", %llu %sfailed" ANSI_RESET
              ", %llu %sskipped\n" ANSI_RESET,
              total_test_count, pass_count, fail_count, fail_color, skip_count,
              skip_color);

end:
    test_info("%s%s" ANSI_RESET " (%llu ms)\n", color, msg, total_time);

#endif
}
