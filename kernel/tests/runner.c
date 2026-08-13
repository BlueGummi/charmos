#include <colors.h>
#include <console/panic.h>
#include <console/printf.h>
#include <console/report.h>
#include <console/statusbar.h>
#include <console/term.h>
#include <crypto/prng.h>
#include <global.h>
#include <irq/irq.h>
#include <math/div.h>
#include <math/sort.h>
#include <mem/alloc_or_die.h>
#include <mem/vas.h>
#include <smp/core.h>
#include <stack_depot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <test.h>
#include <time/spin_sleep.h>
#include <time/time.h>

#include "mem/slab/internal.h"

/* Basically, every test and test setting wires back here */
CMDLINE_ENTRY_DECLARE(test_root, .name = "test",
                      .flags = CMDLINE_ENTRY_SYMBOLIC);

LINKER_SECTION_OBJECT(struct test_group, test_groups)
test_group_orphan_parent = {.name = "test_group_orphan_parent",
                            .enabled = TEST_STATE_SENTINEL,
                            .exit_on_fail = false,
                            .incremental = false,
                            .flags = TEST_GROUP_FLAG_DEFAULT,
                            .ent = CMDLINE_ENTRY(test_root)};

static void test_filter_callback(const char *val, struct cmdline_entry *ent) {
    (void) ent, (void) val;
    test_global.test_opt_in = true;
    test_global.group_opt_in = true;
}

CMDLINE_ENTRY_DECLARE(
    test_filter, .name = "filter",
    .value.accepted = CMDLINE_VALUE_TYPE_BIT(CMDLINE_VAL_STRING) |
                      CMDLINE_VALUE_TYPE_BIT(CMDLINE_VAL_LIST),
    .parent = CMDLINE_ENTRY(test_root), .arg = "<list>",
    .callback = test_filter_callback,
    .desc = "this makes tests and groups opt-in and enables whatever tests "
            "and/or groups are passed in, WITHOUT namespaces, i.e. only "
            "\"test_name\" or \"test_group_name\"",
    .flags = CMDLINE_ENTRY_DOCUMENTED);

CMDLINE_ENTRY_DECLARE_TYPED(
    test_group_opt_in, test_global.group_opt_in, .name = "group_opt_in",
    .parent = CMDLINE_ENTRY(test_root), .arg = CMDLINE_ENTRY_TYPE_TO_ARG(bool),
    .desc = "By default, tests are opt-out, and compiled tests will run, and "
            "this inverts that",
    .flags = CMDLINE_ENTRY_DOCUMENTED);
CMDLINE_ENTRY_DECLARE_TYPED(test_test_opt_in, test_global.test_opt_in,
                            .name = "test_opt_in",
                            .parent = CMDLINE_ENTRY(test_root));

CMDLINE_ENTRY_DECLARE_TYPED(test_show_output, test_global.show_output,
                            .name = "show_output",
                            .parent = CMDLINE_ENTRY(test_root));

CMDLINE_ENTRY_DECLARE_TYPED(
    test_no_exit, test_global.no_exit, .name = "no_exit",
    .parent = CMDLINE_ENTRY(test_root), .arg = CMDLINE_ENTRY_TYPE_TO_ARG(bool),
    .desc = "Idle after the suite completes instead of asking QEMU to exit",
    .flags = CMDLINE_ENTRY_DOCUMENTED);

CMDLINE_ENTRY_DECLARE_TYPED(
    test_no_progress, test_global.no_progress, .name = "no_progress",
    .parent = CMDLINE_ENTRY(test_root), .arg = CMDLINE_ENTRY_TYPE_TO_ARG(bool),
    .desc = "Never pin a progress bar to the bottom of the serial terminal, "
            "even when one answers the size probe",
    .flags = CMDLINE_ENTRY_DOCUMENTED);

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
struct test_globals test_global = {0};

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

static void tests_set_enabled_states() {
#ifdef TEST_ENABLED

    /* Here we just apply the globals */
    if (test_global.test_opt_in) {
        for (struct test *t = __skernel_tests; t < __ekernel_tests; t++) {
            if (t->enabled == TEST_STATE_SENTINEL) {
                t->enabled = TEST_STATE_DISABLED;
            }
        }
    } else {
        for (struct test *t = __skernel_tests; t < __ekernel_tests; t++) {
            if (t->enabled == TEST_STATE_SENTINEL) {
                t->enabled = TEST_STATE_ENABLED;
            }
        }
    }

    if (test_global.group_opt_in) {
        for (struct test_group *tg = __skernel_test_groups;
             tg < __ekernel_test_groups; tg++) {
            if (tg->enabled == TEST_STATE_SENTINEL) {
                tg->enabled = TEST_STATE_DISABLED;
                for (int i = 0; i < TEST_TIER_MAX; i++)
                    tg->tier_enabled[i] = TEST_STATE_DISABLED;
            }
        }

    } else {
        for (struct test_group *tg = __skernel_test_groups;
             tg < __ekernel_test_groups; tg++) {
            if (tg->enabled == TEST_STATE_SENTINEL) {
                tg->enabled = TEST_STATE_ENABLED;
                for (int i = 0; i < TEST_TIER_MAX; i++)
                    tg->tier_enabled[i] = TEST_STATE_ENABLED;
            }
        }
    }

    for (struct test_group *tg = __skernel_test_groups;
         tg < __ekernel_test_groups; tg++) {
        for (int i = 0; i < TEST_TIER_MAX; i++) {
            tg->num_tests_enabled[i] = 0;
            for (size_t j = 0; j < tg->num_tests[i]; j++) {
                struct test *t = tg->tests[i][j];
                if (t->enabled)
                    tg->num_tests_enabled[i]++;
            }
        }
    }

    for (struct test *t = __skernel_tests; t < __ekernel_tests; t++) {
        if (t->enabled)
            test_global.total_tests_enabled++;
    }

#endif
}

struct test_dup_item {
    char *name;    /* Points to the test or test_group->name */
    uint32_t hash; /* hash_murmur3_32() */
};

static int test_dup_item_cmp(const void *a, const void *b) {
    const struct test_dup_item *ta = a;
    const struct test_dup_item *tb = b;

    if (ta->hash < tb->hash)
        return -1;
    else if (ta->hash > tb->hash)
        return 1;
    else
        return strcmp(ta->name, tb->name);
}

/* NOTE:
 * Rule: no test_group or test may share a name with any test or test_group,
 * this is because the --filter build flag can enable tests or test groups,
 * and we can't have that be ambiguous
 */
static void tests_check_duplicate_names() {
    size_t num_tests = __ekernel_tests - __skernel_tests;
    size_t num_test_groups = __ekernel_test_groups - __skernel_test_groups;
    size_t num_items = num_tests + num_test_groups;

    struct test_dup_item *items = kmalloc_or_die(
        sizeof(struct test_dup_item) * num_items, ALLOC_FLAGS_ZERO);

    for (size_t i = 0; i < num_tests; i++) {
        items[i].name = (char *) __skernel_tests[i].name;
        items[i].hash =
            hash_murmur3_32(items[i].name, strlen(items[i].name), 0);
    }

    for (size_t i = 0; i < num_test_groups; i++) {
        items[num_tests + i].name = (char *) __skernel_test_groups[i].name;
        items[num_tests + i].hash = hash_murmur3_32(
            items[num_tests + i].name, strlen(items[num_tests + i].name), 0);
    }

    qsort(items, num_items, sizeof(struct test_dup_item), test_dup_item_cmp);

    for (size_t i = 1; i < num_items; i++) {
        if (items[i].hash == items[i - 1].hash &&
            strcmp(items[i].name, items[i - 1].name) == 0) {
            panic("Duplicate test/test_group name: %s", items[i].name);
        }
    }

    kfree(items);
}

static void test_filter_enable(char *name) {
    /* Duplicate name presence will panic the kernel */
    for (struct test_group *tg = __skernel_test_groups;
         tg < __ekernel_test_groups; tg++) {
        if (strcmp(tg->name, name) == 0) {
            tg->enabled = TEST_STATE_ENABLED;
            return;
        }
    }

    for (struct test *t = __skernel_tests; t < __ekernel_tests; t++) {
        if (strcmp(t->name, name) == 0) {
            t->enabled = TEST_STATE_ENABLED;

            struct test_group *g = (struct test_group *) t->group;
            g->enabled = TEST_STATE_ENABLED;
            return;
        }
    }

    panic("%s in the filter is not a valid test group or test name", name);
}

static void tests_apply_filters() {
    struct cmdline_entry *filter = CMDLINE_ENTRY(test_filter);
    if (filter->status != CMDLINE_ENTRY_FOUND)
        return;

    if (filter->value.type == CMDLINE_VAL_STRING) {
        char *filter_one;
        CMDLINE_EXTRACT(&filter->value, filter_one);
        test_filter_enable(filter_one);
    } else {
        kassert(filter->value.type == CMDLINE_VAL_LIST);
        struct cmdline_list list;
        CMDLINE_EXTRACT(&filter->value, list);
        struct cmdline_value val;
        cmdline_list_for_each(val, &list) {
            kassert(val.type == CMDLINE_VAL_STRING);
            char *filter_one;
            CMDLINE_EXTRACT(&val, filter_one);
            test_filter_enable(filter_one);
        }
    }
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

static struct {
    size_t total;
    size_t done;
    size_t failed;
    size_t skipped;
} test_progress = {0};

static size_t tests_count_planned(void) {
    size_t n = 0;

    for (struct test_group *tg = __skernel_test_groups;
         tg < __ekernel_test_groups; tg++) {
        if (!tg->enabled)
            continue;

        for (int i = 0; i < TEST_TIER_MAX; i++) {
            if (tg->tier_enabled[i])
                n += tg->num_tests[i];
        }
    }

    return n;
}

static void test_progress_paint(const struct test_group *tg,
                                enum test_tier tier, const char *test_name) {
    char tail[96] = "";

    if (!test_progress.total)
        return;

    if (test_progress.failed || test_progress.skipped)
        snprintf(tail, (int) sizeof(tail),
                 "  " ANSI_RED "%zu failed" ANSI_RESET " " ANSI_GRAY
                 "%zu skipped" ANSI_RESET,
                 test_progress.failed, test_progress.skipped);

    status_bar_progress(test_progress.done, test_progress.total,
                        ANSI_BLUE "%s" ANSI_RESET " (%s" ANSI_RESET
                                  ") " ANSI_BOLD "%s" ANSI_RESET "%s",
                        tg->name, test_tier_to_str_color(tier), test_name,
                        tail);
}

static void test_handle_print(const struct log_site *site,
                              const struct log_record *rec,
                              void (*print)(const char *fmt, ...)) {
    (void) site;
    print("[%s%zu.%03zu" ANSI_RESET "] ", log_level_color(rec->level),
          MS_TO_SECONDS(rec->timestamp), rec->timestamp % 1000);
}

static void test_group_run(struct test_group *tg) {
    if (!tg->enabled)
        return;

    bool all_disabled = true;
    for (int i = 0; i < TEST_TIER_MAX; i++) {
        if (tg->tier_enabled[i]) {
            all_disabled = false;
            break;
        }
    }

    if (all_disabled)
        return;

    bool no_tests = true;
    for (int i = 0; i < TEST_TIER_MAX; i++) {
        if (tg->num_tests_enabled[i]) {
            no_tests = false;
            break;
        }
    }

    if (no_tests)
        return;

    /* TODO: A little hacky */
    size_t total_tests = 0;
    time_ms_t total_time = 0;
    for (int i = 0; i < TEST_TIER_MAX; i++)
        total_tests += tg->num_tests_enabled[i];

    test_harness_info(ANSI_GREEN ANSI_BOLD
                      "Running" ANSI_RESET " group " ANSI_BLUE ANSI_BOLD
                      "%s" ANSI_RESET " - %zu tests in (" ANSI_BOLD
                      "%s" ANSI_RESET ")\n",
                      tg->name, total_tests, tg->fname);
    printf("%*s  | ", 20, "");
    printf(tg->incremental ? "incremental, " : "non_incremental, ");
    printf(tg->exit_on_fail ? "exit_on_fail" : "continue_on_fail");
    printf("\n");
    printf("%*s  | ", 20, "");
    printf(ANSI_UNDERLINE ANSI_BOLD "enabled" ANSI_RESET ": ");
    for (int i = 0; i < TEST_TIER_MAX; i++) {
        if (!tg->num_tests_enabled[i])
            continue;

        if (tg->tier_enabled[i])
            printf(ANSI_BOLD "%s" ANSI_RESET, test_tier_to_str_color(i));

        /* Check if the next one exists to print a comma */
        if (i + 1 < TEST_TIER_MAX && tg->tier_enabled[i + 1] &&
            tg->num_tests_enabled[i + 1])
            printf(", ");
    }

    printf("\n");

    bool stop_outer = false;
    *LOG_SITE(test_harness).name = (char *) tg->name;

    struct test_group_result result_totals = {0};
    size_t result_aggregates[TEST_RESULT_MAX] = {0};
    for (int i = 0; i < TEST_TIER_MAX; i++) {
        if (stop_outer)
            break;

        if (!tg->num_tests_enabled[i] || !tg->tier_enabled[i])
            continue;

        const char *tier_name = test_tier_to_str_color(i);
        size_t len = snprintf(NULL, 0,
                              "%s " ANSI_RESET "(" ANSI_BOLD ANSI_MAGENTA
                              "%s" ANSI_RESET ")" ANSI_GREEN,
                              tg->name, tier_name) +
                     1;
        char *name = kmalloc_or_die(len);
        snprintf(name, len,
                 "%s " ANSI_RESET "(" ANSI_BOLD ANSI_MAGENTA "%s" ANSI_RESET
                 ")" ANSI_GREEN,
                 tg->name, tier_name);
        *LOG_SITE(test_harness).name = (char *) name;

        for (size_t test_num = 0; test_num < tg->num_tests[i]; test_num++) {
            struct test *t = tg->tests[i][test_num];
            struct test_context tctx = {0};
            if (!t->enabled)
                continue;

            /* Modifiable by the test */
            struct log_dump_options dopts = {
                .min_level = LOG_TRACE,
                .show_args = true,
            };

            enum log_site_flags flags = LOG_SITE_NONE;
            if (t->print_logs && test_global.show_output)
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
            tctx.handle.print = test_handle_print;
            tctx.intensity = t->intensity;
            tctx.seed = !t->seed ? prng_next() : t->seed;

            size_t result_times[TEST_RESULT_MAX] = {0}, run_times = 0;

            struct test_verdict *verdicts = kmalloc_or_die(
                sizeof(struct test_verdict) * t->run_times, ALLOC_FLAGS_ZERO);
            struct test_verdict singular_verdict = {0};

            test_harness_info(ANSI_BOLD ANSI_BLUE "%s" ANSI_RESET, t->name);
            test_progress_paint(tg, i, t->name);

            time_ms_t start_ms = time_get_ms();
            for (; run_times < t->run_times; run_times++) {
                struct test_verdict verdict = t->func(&tctx);
                singular_verdict = verdict;
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
            time_ms_t end_ms = time_get_ms();
            time_ms_t took = end_ms - start_ms;
            total_time += took;
            test_global.total_time += took;

            size_t non_skipped = run_times - result_times[TEST_RESULT_SKIPPED];

            if (t->run_times > 1) {
                char *color = non_skipped < run_times ? ANSI_RED : ANSI_BLUE;
                printf(" ran (%s%zu" ANSI_RESET "/" ANSI_BLUE "%zu" ANSI_RESET
                       ") times in " ANSI_BRIGHT_WHITE "%zu" ANSI_RESET " ms,",
                       color, non_skipped, t->run_times, took);
                if (result_times[TEST_RESULT_OK] == run_times) {
                    printf(ANSI_BLUE " all successful" ANSI_RESET);
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
                        test_harness_info("  |-> run %zu %s", i,
                                          test_result_to_str(v.result));
                        if (v.result == TEST_RESULT_SKIPPED) {
                            printf(ANSI_GRAY " (%s)" ANSI_RESET,
                                   test_skip_reason_to_str(v.skip_reason));
                        } else if (v.msg) {
                            printf(ANSI_RED " (%s)" ANSI_RESET, v.msg);
                        }
                        printf("\n");
                    }
                }
            } else {
                char *status;
                char *color;
                switch (singular_verdict.result) {
                case TEST_RESULT_OK:
                    color = ANSI_GREEN ANSI_BOLD;
                    status = "successful";
                    break;
                case TEST_RESULT_SKIPPED:
                    color = ANSI_GRAY ANSI_BOLD;
                    status = "skipped";
                    break;
                case TEST_RESULT_FAILED:
                    color = ANSI_RED ANSI_BOLD;
                    status = "error";
                    break;
                default: kassert_unreachable();
                }
                printf(" %s%s" ANSI_RESET " in " ANSI_BOLD "%zu" ANSI_RESET
                       " ms",
                       color, status, took);
                if (singular_verdict.result == TEST_RESULT_SKIPPED)
                    printf(
                        " (reason: " ANSI_GRAY "%s" ANSI_RESET ")",
                        test_skip_reason_to_str(singular_verdict.skip_reason));
                else if (singular_verdict.result == TEST_RESULT_FAILED &&
                         singular_verdict.msg)
                    printf(" (" ANSI_RED "%s" ANSI_RESET ")",
                           singular_verdict.msg);

                printf("\n");
            }

            if ((log_site_message_count(tctx.site) &&
                 (result_times[TEST_RESULT_FAILED] ||
                  (result_times[TEST_RESULT_SKIPPED] && t->run_times > 1))) ||
                test_global.show_output) {
                test_harness_info("messages:\n");
                log_dump_site(tctx.site);
            }

            test_progress.done++;
            if (result_times[TEST_RESULT_FAILED])
                test_progress.failed++;
            else if (result_times[TEST_RESULT_SKIPPED] == run_times)
                test_progress.skipped++;

            test_progress_paint(tg, i, t->name);

            for (int j = 0; j < TEST_RESULT_MAX; j++)
                result_totals.totals[i][j] += result_times[j];

            kfree(verdicts);
            if (result_times[TEST_RESULT_FAILED] && tg->incremental) {
                stop_outer = true;
            } else if (result_times[TEST_RESULT_FAILED] && tg->exit_on_fail) {
                stop_outer = true;
                break;
            }
        }

        kfree(name);
    }

    *LOG_SITE(test_harness).name = "test_harness";
    for (int i = 0; i < TEST_TIER_MAX; i++) {
        for (int j = 0; j < TEST_RESULT_MAX; j++) {
            test_global.results[i][j] += result_totals.totals[i][j];
            result_aggregates[j] += result_totals.totals[i][j];
        }
    }

    if (!result_aggregates[TEST_RESULT_SKIPPED] &&
        !result_aggregates[TEST_RESULT_FAILED]) {
        test_harness_info("Test group " ANSI_BLUE ANSI_BOLD "%s" ANSI_RESET
                          " " ANSI_GREEN ANSI_BOLD "successful" ANSI_RESET
                          " in " ANSI_BOLD "%zu" ANSI_RESET " ms\n\n\n",
                          tg->name, total_time);
        *LOG_SITE(test_harness).name = "test_harness";
    } else {
        test_harness_info("Test group " ANSI_BLUE ANSI_BOLD "%s" ANSI_RESET
                          " completed in " ANSI_BOLD "%zu" ANSI_RESET " ms, ",
                          tg->name, total_time);

        if (result_aggregates[TEST_RESULT_SKIPPED])
            printf("%zu " ANSI_GRAY ANSI_BOLD "skipped" ANSI_RESET,
                   result_aggregates[TEST_RESULT_SKIPPED]);

        if (result_aggregates[TEST_RESULT_FAILED])
            printf(", %zu " ANSI_RED ANSI_BOLD "failed" ANSI_RESET,
                   result_aggregates[TEST_RESULT_FAILED]);

        printf("\n\n\n");
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
    tests_check_duplicate_names();
    tests_setup_groups();
    tests_apply_filters();
    tests_set_enabled_states();

    bool all_ok = true;
    char *msg = all_ok ? "all tests pass 🎉!" : "some errors occurred";
    char *color = all_ok ? ANSI_GREEN : ANSI_RED;

    test_harness_info("Running " ANSI_BOLD "%zu" ANSI_RESET " tests:\n",
                      test_global.total_tests_enabled);

    if (!test_global.no_progress && term_probe()) {
        test_progress.total = tests_count_planned();
        status_bar_open();
        status_bar_progress(0, test_progress.total, "starting");
    }

    for (struct test_group *tg = __skernel_test_groups;
         tg < __ekernel_test_groups; tg++)
        test_group_run(tg);

    status_bar_close();
    test_global_aggregate_results();

    size_t fail_count = test_global.results_agg[TEST_RESULT_FAILED];
    size_t skip_count = test_global.results_agg[TEST_RESULT_SKIPPED];
    size_t pass_count = test_global.results_agg[TEST_RESULT_OK];
    time_ms_t total_time = test_global.total_time;
    all_ok = fail_count == 0;
    color = all_ok ? ANSI_GREEN : ANSI_RED;
    msg = all_ok ? "all tests pass 🎉!" : "some errors occurred";
    char *fail_color = all_ok ? ANSI_GREEN : ANSI_RED;
    char *skip_color = all_ok ? ANSI_GREEN : ANSI_GRAY;

    test_harness_info("%llu " ANSI_CYAN "total" ANSI_RESET
                      " tests, %llu " ANSI_GREEN "passed" ANSI_RESET
                      ", %llu %sfailed" ANSI_RESET ", %llu %sskipped" ANSI_RESET
                      "\n",
                      test_global.total_tests_enabled, pass_count, fail_count,
                      fail_color, skip_count, skip_color);

    test_harness_info("%s%s" ANSI_RESET " (%llu ms)\n", color, msg, total_time);

    /* Give it the return code */
    if (!test_global.no_exit)
        qemu_exit(all_ok ? TEST_EXIT_OK : TEST_EXIT_FAIL);
#endif
}
