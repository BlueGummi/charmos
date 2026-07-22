#include <colors.h>
#include <console/panic.h>
#include <console/printf.h>
#include <crypto/prng.h>
#include <global.h>
#include <irq/irq.h>
#include <math/div.h>
#include <mem/alloc_or_die.h>
#include <mem/vas.h>
#include <smp/core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <test.h>
#include <time/spin_sleep.h>
#include <time/time.h>

#include "mem/slab/internal.h"

/* Basically, every test and test setting wires back here */
CMDLINE_ENTRY_DECLARE(test_root, .name = "test");
LINKER_SECTION_OBJECT(struct test_group, test_groups)
test_group_orphan_parent = {.name = "test_group_orphan_parent",
                            .enabled = true,
                            .exit_on_fail = false,
                            .ent = CMDLINE_ENTRY(test_root)};

LOG_SITE_DECLARE_DEFAULT(test_harness);
LOG_HANDLE_DECLARE_DEFAULT(test_harness, .flags = LOG_PRINT | LOG_NO_NEWLINE);

#define test_harness_log(lvl, fmt, ...)                                        \
    log(LOG_SITE(test_harness), LOG_HANDLE(test_harness), lvl, fmt,            \
        ##__VA_ARGS__)

#define test_harness_err(fmt, ...)                                             \
    test_harness_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define test_harness_warn(fmt, ...)                                            \
    test_harness_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define test_harness_info(fmt, ...)                                            \
    test_harness_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define test_harness_debug(fmt, ...)                                           \
    test_harness_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define test_harness_trace(fmt, ...)                                           \
    test_harness_log(LOG_TRACE, fmt, ##__VA_ARGS__)

LINKER_SECTION_DEFINE(struct test, tests);
LINKER_SECTION_DEFINE(struct test_group, test_groups);
/* no need to clean up allocations in these tests, we are supposed to
 * reboot/poweroff after all tests complete, and the userland should
 * not be in a state where we can boot it when running tests */
struct test_globals test_global;

static bool is_keyword(const char *check, const char **against,
                       size_t num_keywords) {
    for (size_t i = 0; i < num_keywords; i++) {
        if (strcmp(check, against[i]) == 0)
            return true;
    }

    return false;
}

void tests_hook_boot() {
#ifdef TEST_ENABLED
    /* In here, we go through all tests, and assign their command line parents
     */
    for (struct test *t = __skernel_tests; t < __ekernel_tests; t++) {
        kassert(t->group);
        t->base_entry->parent = t->group->ent;
    }
#endif
}

static void tests_setup_groups() {
    for (struct test_group *tg = __skernel_test_groups;
         tg < __ekernel_test_groups; tg++) {
        size_t num_tests[TEST_TIER_MAX] = {0};
        for (struct test *t = __skernel_tests; t < __ekernel_tests; t++) {
            if (t->group == tg) {
                num_tests[t->tier]++;
            }
        }

        for (int i = 0; i < TEST_TIER_MAX; i++) {
            if (num_tests[i]) {
                tg->tests[i] =
                    kmalloc_or_die(sizeof(struct test *) * num_tests[i]);
            }
            tg->num_tests[i] = num_tests[i];
        }

        for (struct test *t = __skernel_tests; t < __ekernel_tests; t++) {
            if (t->group == tg) {
                tg->tests[t->tier][num_tests[t->tier] - 1] = t;
                num_tests[t->tier]--;
            }
        }
    }
}

struct test_group_result {
    size_t totals[TEST_TIER_MAX][TEST_RESULT_MAX];
};

static void test_group_run(struct test_group *tg) {
    if (!tg->enabled)
        return;

    size_t total_tests = 0;
    time_t total_time = 0;
    for (int i = 0; i < TEST_TIER_MAX; i++)
        total_tests += tg->num_tests[i];

    test_harness_info("Running test group '%s' (%zu tests):\n", tg->name,
                      total_tests);
    test_harness_info("  incremental: %s\n",
                      tg->incremental ? "true" : "false");
    test_harness_info("  exit_on_fail: %s\n",
                      tg->exit_on_fail ? "true" : "false");
    test_harness_info("  default intensity: %F\n", tg->default_intensity);

    bool stop_outer = false;

    struct test_group_result result_totals = {0};
    for (int i = 0; i < TEST_TIER_MAX; i++) {
        if (stop_outer)
            break;

        test_harness_info("%zu %s tests:\n", tg->num_tests[i],
                          test_tier_to_str(i));
        if (!tg->num_tests[i])
            continue;

        for (size_t test_num = 0; test_num < tg->num_tests[i]; test_num++) {
            struct test *t = tg->tests[i][test_num];
            struct test_context tctx = {0};

            /* Modifiable by the test */
            struct log_dump_options dopts = {
                .min_level = LOG_TRACE,
                .show_args = true,
            };

            enum log_site_flags flags = LOG_SITE_NONE;
            if (t->print_logs)
                flags |= LOG_SITE_PRINT;

            struct log_site_options opts = {
                .capacity =
                    t->msg_cap == 0 ? LOG_SITE_CAPACITY_DEFAULT : t->msg_cap,
                .name = "test",
                .enabled_mask = LOG_SITE_ALL,
                .dump_opts = dopts,
                .flags = flags,
            };
            alloc_or_die(tctx.site = log_site_create(opts));
            test_global.current_test = &tctx;
            tctx.handle.msg = "test_handle";
            tctx.intensity = t->intensity;
            tctx.seed = !t->seed ? prng_next() : t->seed;

            size_t result_times[TEST_RESULT_MAX] = {0}, run_times = 0;
            struct test_verdict verdicts[t->run_times];

            enum test_result singular_result = TEST_RESULT_MAX;

            test_harness_info("'%s'", t->name);

            time_t start_ms = time_get_ms();
            for (; run_times < t->run_times; run_times++) {
                struct test_verdict verdict = t->func(&tctx);
                singular_result = verdict.result;
                verdicts[run_times] = verdict;

                if (verdict.result == TEST_RESULT_SKIPPED) {
                    result_times[TEST_RESULT_SKIPPED]++;
                } else if (verdict.result == TEST_RESULT_FAILED) {
                    result_times[TEST_RESULT_FAILED]++;
                } else {
                    result_times[TEST_RESULT_OK]++;
                }

                if (result_times[TEST_RESULT_FAILED] && !t->keep_going)
                    break;

                tctx.seed = !t->seed ? prng_next() : t->seed;
            }
            time_t end_ms = time_get_ms();
            time_t took = end_ms - start_ms;
            total_time += took;

            size_t total_run = run_times - result_times[TEST_RESULT_SKIPPED];

            if (t->run_times > 1) {
                char *color = run_times < total_run ? ANSI_RED : ANSI_BLUE;
                printf(" ran (%s%zu" ANSI_RESET "/" ANSI_BLUE "%zu" ANSI_RESET
                       ") times in " ANSI_GREEN "%zu" ANSI_RESET " ms,",
                       color, total_run, t->run_times, took);
                if (result_times[TEST_RESULT_OK] == run_times) {
                    printf(ANSI_BLUE " all successful" ANSI_RESET "\n");
                } else if (result_times[TEST_RESULT_SKIPPED]) {
                    printf(ANSI_GRAY " %zu skipped" ANSI_RESET,
                           result_times[TEST_RESULT_SKIPPED]);
                }

                if (result_times[TEST_RESULT_FAILED]) {
                    printf(ANSI_RED " %zu failed" ANSI_RESET "\n",
                           result_times[TEST_RESULT_FAILED]);
                }

                for (size_t i = 0; i < run_times; i++) {
                    struct test_verdict v = verdicts[i];
                    if (v.result != TEST_RESULT_OK) {
                        test_harness_info("  |-> run %zu %s", i, v.result);
                        if (v.result == TEST_RESULT_SKIPPED) {
                            printf(ANSI_GRAY " (%s)" ANSI_RESET, v.skip_reason);
                        }
                        printf("\n");
                    }
                }
            } else {
                char *status;
                char *color;
                switch (singular_result) {
                case TEST_RESULT_OK:
                    color = ANSI_BLUE;
                    status = "successful";
                    break;
                case TEST_RESULT_SKIPPED:
                    color = ANSI_GRAY;
                    status = "skipped";
                    break;
                case TEST_RESULT_FAILED:
                    color = ANSI_RED;
                    status = "error";
                    break;
                default: kassert_unreachable();
                }
                printf(" %s%s" ANSI_RESET " in " ANSI_GREEN "%zu" ANSI_RESET
                       " ms\n",
                       color, status, took);
            }

            if (log_site_message_count(tctx.site) && !t->print_logs) {
                test_harness_info("messages:\n");
                log_dump_site(tctx.site);
            }

            for (int j = 0; j < TEST_RESULT_MAX; j++)
                result_totals.totals[i][j] += result_times[j];

            if (result_times[TEST_RESULT_FAILED] && tg->incremental) {
                stop_outer = true;
            } else if (result_times[TEST_RESULT_FAILED] && tg->exit_on_fail) {
                stop_outer = true;
                break;
            }
        }
    }

    for (int i = 0; i < TEST_TIER_MAX; i++) {
        for (int j = 0; j < TEST_RESULT_MAX; j++) {
            test_global.results[i][j] += result_totals.totals[i][j];
        }
    }

    test_harness_info("Test group '%s' complete in %zu ms: \n", tg->name,
                      total_time);
    if (stop_outer) {
        for (int i = 0; i < TEST_TIER_MAX; i++) {
            size_t fails = result_totals.totals[i][TEST_RESULT_FAILED];
            size_t skips = result_totals.totals[i][TEST_RESULT_SKIPPED];
            if (skips && !fails) {
                test_harness_info("  Tier '%s' had %zu skips\n",
                                  test_tier_to_str(i), skips);
            } else if (fails && !skips) {
                test_harness_info("  Tier '%s' had %zu fails\n",
                                  test_tier_to_str(i), fails);
            } else {
                test_harness_info("  Tier '%s' had %zu skips, %zu fails\n",
                                  test_tier_to_str(i), skips, fails);
            }
        }
    } else {
        test_harness_info("  All successful\n");
    }
}

static void test_global_aggregate_results() {
    for (int i = 0; i < TEST_TIER_MAX; i++) {
        for (int j = 0; j < TEST_RESULT_MAX; j++) {
            test_global.results_agg[j] += test_global.results[i][j];
        }
    }
}

void tests_run(void) {
#ifdef TEST_ENABLED
    tests_setup_groups();

    bool all_ok = true;
    char *msg = all_ok ? "all tests pass 🎉!" : "some errors occurred";
    char *color = all_ok ? ANSI_GREEN : ANSI_RED;

    test_harness_info("Running %zu tests:\n",
                      __ekernel_tests - __skernel_tests);

    for (struct test_group *tg = __skernel_test_groups;
         tg < __ekernel_test_groups; tg++) {
        test_group_run(tg);
    }
    test_global_aggregate_results();

    size_t fail_count = test_global.results_agg[TEST_RESULT_FAILED];
    size_t skip_count = test_global.results_agg[TEST_RESULT_SKIPPED];
    size_t pass_count = test_global.results_agg[TEST_RESULT_OK];
    time_t total_time = test_global.total_time;
    all_ok = fail_count == 0;
    color = all_ok ? ANSI_GREEN : ANSI_RED;
    msg = all_ok ? "all tests pass 🎉!" : "some errors occurred";
    char *fail_color = all_ok ? ANSI_GREEN : ANSI_RED;
    char *skip_color = all_ok ? ANSI_GREEN : ANSI_GRAY;

    test_harness_info("%llu " ANSI_CYAN "total" ANSI_RESET
                      " tests, %llu " ANSI_GREEN "passed" ANSI_RESET
                      ", %llu %sfailed" ANSI_RESET ", %llu %sskipped" ANSI_RESET
                      "\n",
                      __ekernel_tests - __skernel_tests, pass_count, fail_count,
                      fail_color, skip_count, skip_color);

    test_harness_info("%s%s" ANSI_RESET " (%llu ms)\n", color, msg, total_time);

    vas_space_dump(slab_global.vas);

#endif
}
