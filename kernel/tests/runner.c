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
#include <ndjson.h>
#include <smp/core.h>
#include <stack_depot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <test/export.h>
#include <test/test.h>
#include <time/spin_sleep.h>
#include <time/time.h>

#include "mem/slab/internal.h"

/* Basically, every test and test setting wires back here */
CMDLINE_DECLARE(test_root, .name = "test", .flags = CMDLINE_ENTRY_SYMBOLIC);

LINKER_SECTION_OBJECT(struct test_group, test_groups)
test_group_orphan_parent = {.name = "test_group_orphan_parent",
                            .enabled = TEST_STATE_SENTINEL,
                            .exit_on_fail = false,
                            .incremental = false,
                            .flags = TEST_GROUP_FLAG_DEFAULT};

CMDLINE_CHILDREN_DECLARE(
    test_root,
    CMDLINE_INNER(
        filter, .types = CMDLINE_TYPES(CMDLINE_TYPE_STRING, CMDLINE_TYPE_LIST),
        .desc = "this makes tests and groups opt-in and enables whatever tests "
                "and/or groups are passed in, WITHOUT namespaces, i.e. only "
                "\"test_name\" or \"test_group_name\""),
    CMDLINE_INNER_FX(global_intensity, test_global.global_intensity,
                     .desc = "global intensity override for all tests",
                     .range = RANGE(0, FX_ONE)),
    CMDLINE_INNER_VAR(
        group_opt_in, test_global.group_opt_in,
        .desc = "By default, tests are opt-out, and compiled tests will run, "
                "and this inverts that"),
    CMDLINE_INNER_VAR(test_opt_in, test_global.test_opt_in,
                      .flags = CMDLINE_ENTRY_HIDDEN),
    CMDLINE_INNER_VAR(show_output, test_global.show_output,
                      .flags = CMDLINE_ENTRY_HIDDEN),
    CMDLINE_INNER_VAR(no_exit, test_global.no_exit,
                      .desc = "Idle after the suite completes"),
    CMDLINE_INNER_VAR(no_progress, test_global.no_progress,
                      .desc = "Do not show progress bar"));

NDJSON_DECLARE(test_begin, NDJSON_DOMAIN_TEST, NDJSON_KIND_BEGIN, 1,
               NDJSON_U64(declared_total));

NDJSON_DECLARE(test_group_start, NDJSON_DOMAIN_TEST, NDJSON_KIND_GROUP_START, 1,
               NDJSON_STR(group), NDJSON_U64(test_count), NDJSON_STR(file));

NDJSON_DECLARE(test_result, NDJSON_DOMAIN_TEST, NDJSON_KIND_RESULT, 1,
               NDJSON_STR(group), NDJSON_STR(tier), NDJSON_STR(name),
               NDJSON_STR(status), NDJSON_U64(duration_ms), NDJSON_STR(reason),
               NDJSON_STR(msg), NDJSON_U64(runs_requested),
               NDJSON_U64(runs_attempted), NDJSON_U64(runs_failed),
               NDJSON_U64(runs_skipped));

NDJSON_DECLARE(test_group_end, NDJSON_DOMAIN_TEST, NDJSON_KIND_GROUP_END, 1,
               NDJSON_STR(group), NDJSON_U64(duration_ms), NDJSON_U64(failed),
               NDJSON_U64(skipped));

NDJSON_DECLARE(test_totals, NDJSON_DOMAIN_TEST, NDJSON_KIND_TOTALS, 1,
               NDJSON_U64(total), NDJSON_U64(passed), NDJSON_U64(failed),
               NDJSON_U64(skipped));

NDJSON_DECLARE(test_verdict, NDJSON_DOMAIN_TEST, NDJSON_KIND_VERDICT, 1,
               NDJSON_BOOL(ok), NDJSON_U64(duration_ms));

static const char *test_status_plain(enum test_result r) {
    switch (r) {
    case TEST_RESULT_OK: return "pass";
    case TEST_RESULT_FAILED: return "fail";
    case TEST_RESULT_SKIPPED: return "skip";
    default: return "unknown";
    }
}

static const char *test_tier_plain(enum test_tier t) {
    switch (t) {
    case TEST_TIER_SMOKE: return "smoke";
    case TEST_TIER_UNIT: return "unit";
    case TEST_TIER_INTEGRATION: return "integration";
    default: return "unknown";
    }
}

LOG_SITE_DECLARE_PRINT(test_harness);
LOG_HANDLE_DECLARE_PRINT(test_harness, .flags = LOG_PRINT | LOG_NO_NEWLINE);

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
struct test_globals test_global = {.global_intensity = TEST_INTENSITY_SENTINEL};

static void *test_instance_resolver(const char *path, size_t path_len) {
    char name_buf[CMDLINE_ENTRY_NAME_LEN_MAX];
    if (path_len >= sizeof(name_buf))
        return NULL;
    memcpy(name_buf, path, path_len);
    name_buf[path_len] = '\0';

    char *last_dot = strrchr(name_buf, '.');
    const char *test_name = NULL;
    const char *group_name = NULL;

    if (last_dot) {
        *last_dot = '\0';
        group_name = name_buf;
        test_name = last_dot + 1;
    } else {
        test_name = name_buf;
    }

    for (struct test *t = __skernel_tests; t < __ekernel_tests; t++) {
        if (strcmp(t->name, test_name) == 0) {
            if (!group_name ||
                (t->group && strcmp(t->group->name, group_name) == 0))
                return t;
        }
    }
    return NULL;
}

static void *test_group_instance_resolver(const char *path, size_t path_len) {
    char name_buf[CMDLINE_ENTRY_NAME_LEN_MAX];
    if (path_len >= sizeof(name_buf))
        return NULL;
    memcpy(name_buf, path, path_len);
    name_buf[path_len] = '\0';

    for (struct test_group *g = __skernel_test_groups;
         g < __ekernel_test_groups; g++) {
        if (strcmp(g->name, name_buf) == 0)
            return g;
    }
    return NULL;
}

CMDLINE_SCHEMA_DECLARE(
    test_props, "test", "<group>.<name>", "Test parameters",
    test_instance_resolver, CMDLINE_SCHEMA_PROP(struct test, enabled),
    CMDLINE_SCHEMA_PROP_FX(struct test, intensity,
                           .desc = "Execution intensity",
                           .range = RANGE(0, FX_ONE)),
    CMDLINE_SCHEMA_PROP(struct test, run_times, .desc = "Times to run the test",
                        .range = RANGE(1, 100000)),
    CMDLINE_SCHEMA_PROP(struct test, seed, .desc = "PRNG seed (TODO:)"),
    CMDLINE_SCHEMA_PROP(
        struct test, duration_ms,
        .desc = "Maximum runtime limit in milliseconds (TODO: DURATION)"),
    CMDLINE_SCHEMA_PROP(struct test, msg_cap, .desc = "Log limit"),
    CMDLINE_SCHEMA_PROP(
        struct test, keep_going,
        .desc = "If one test fails when run_times > 1, keep going"),
    CMDLINE_SCHEMA_PROP(struct test, print_logs,
                        .desc = "Print logs in real time"));

CMDLINE_SCHEMA_DECLARE(
    test_group_props, "test_group", "<group>", "Test group parameters",
    test_group_instance_resolver,
    CMDLINE_SCHEMA_PROP(struct test_group, enabled),
    CMDLINE_SCHEMA_PROP(struct test_group, smoke_enabled),
    CMDLINE_SCHEMA_PROP(struct test_group, unit_enabled),
    CMDLINE_SCHEMA_PROP(struct test_group, integration_enabled),
    CMDLINE_SCHEMA_PROP(
        struct test_group, incremental,
        .desc = "Run tiers incrementally (smoke -> unit -> integration)"),
    CMDLINE_SCHEMA_PROP(struct test_group, exit_on_fail,
                        .desc = "Exit after first test fails"));

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

            /* Enable all children if the group is on */
            for (int i = 0; i < TEST_TIER_MAX; i++) {
                if (tg->tier_enabled[i] == TEST_STATE_SENTINEL)
                    tg->tier_enabled[i] = TEST_STATE_ENABLED;
            }

            for (struct test *t = __skernel_tests; t < __ekernel_tests; t++) {
                if (t->group == tg && t->enabled == TEST_STATE_SENTINEL)
                    t->enabled = TEST_STATE_ENABLED;
            }

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
    struct cmdline_entry *filter = CMDLINE_CHILD(test_root, filter);
    if (filter->status != CMDLINE_ENTRY_FOUND)
        return;

    /* A filter is the opt-in switch, as the option's own description says.
     * Without this everything still sentinel stays enabled and the filter only
     * ever adds, which makes test.filter= silently run the whole suite */
    test_global.test_opt_in = true;
    test_global.group_opt_in = true;

    if (filter->value.type == CMDLINE_TYPE_STRING) {
        char *filter_one;
        CMDLINE_EXTRACT(&filter->value, filter_one);
        test_filter_enable(filter_one);
    } else {
        kassert(filter->value.type == CMDLINE_TYPE_LIST);
        struct cmdline_list list;
        CMDLINE_EXTRACT(&filter->value, list);
        struct cmdline_value val;
        cmdline_list_for_each(val, &list) {
            kassert(val.type == CMDLINE_TYPE_STRING);
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

    /* HACK: find a cleaner way to represent this */
    size_t total_tests = 0;
    time_ms_t total_time = 0;
    for (int i = 0; i < TEST_TIER_MAX; i++)
        total_tests += tg->num_tests_enabled[i];

    test_harness_info(ANSI_GREEN ANSI_BOLD
                      "Running" ANSI_RESET " group " ANSI_BLUE ANSI_BOLD
                      "%s" ANSI_RESET " - %zu tests in (" ANSI_BOLD
                      "%s" ANSI_RESET ")\n",
                      tg->name, total_tests, tg->fname);

    ndjson_emit(test_group_start, .group = tg->name, .test_count = total_tests,
                .file = tg->fname);
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
        for (int j = i + 1; j < TEST_TIER_MAX; j++) {
            if (tg->tier_enabled[j] && tg->num_tests_enabled[j]) {
                printf(", ");
                break;
            }
        }
    }

    printf("\n");

    bool stop_outer = false;
    LOG_SITE(test_harness)->name = (char *) tg->name;

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

        LOG_SITE(test_harness)->name = (char *) name;

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
            tctx.site = alloc_or_die(log_site_create(opts));
            test_global.current_test = &tctx;
            tctx.handle.msg = "test_handle";
            tctx.handle.print = test_handle_print;
            tctx.intensity = t->intensity;
            tctx.seed = !t->seed ? prng_next() : t->seed;

            if (t->flags & TEST_FLAG_HONORS_INTENSITY) {
                tctx.intensity_val =
                    scaled_param_eval(&t->intensity_desc, tctx.intensity);
            } else {
                tctx.intensity_val = 0;
            }

            size_t result_times[TEST_RESULT_MAX] = {0}, run_times = 0;

            struct test_verdict *verdicts = kmalloc_or_die(
                sizeof(struct test_verdict) * t->run_times, ALLOC_FLAGS_ZERO);
            struct test_verdict singular_verdict = {0};

            test_harness_info(ANSI_BOLD ANSI_BLUE "%s" ANSI_RESET, t->name);
            if (t->flags & TEST_FLAG_HONORS_INTENSITY) {
                char intst_str[512] = {0};
                scaled_param_format(&t->intensity_desc, tctx.intensity,
                                    tctx.intensity_val, intst_str,
                                    sizeof(intst_str));
                printf(" [" ANSI_CYAN "%s" ANSI_RESET "]", intst_str);
            }
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

            {
                struct test_verdict *worst = &singular_verdict;
                for (size_t k = 0; k < run_times; k++) {
                    if (verdicts[k].result == TEST_RESULT_FAILED) {
                        worst = &verdicts[k];
                        break;
                    }
                }

                const char *status = test_status_plain(worst->result);
                if (t->run_times > 1 && result_times[TEST_RESULT_FAILED] &&
                    result_times[TEST_RESULT_FAILED] < run_times)
                    status = "flaky";

                ndjson_emit(
                    test_result, .group = tg->name,
                    .tier = test_tier_plain(t->tier), .name = t->name,
                    .status = status, .duration_ms = took,
                    .reason = worst->result == TEST_RESULT_SKIPPED
                                  ? test_skip_reason_to_str(worst->skip_reason)
                                  : NULL,
                    .msg = worst->msg,
                    .runs_requested = t->run_times > 1 ? t->run_times : 0,
                    .runs_attempted = t->run_times > 1 ? run_times : 0,
                    .runs_failed = result_times[TEST_RESULT_FAILED],
                    .runs_skipped = result_times[TEST_RESULT_SKIPPED]);
            }

            bool has_msg = log_site_message_count(tctx.site) > 0;
            bool of_interest =
                result_times[TEST_RESULT_FAILED] ||
                (result_times[TEST_RESULT_SKIPPED] && t->run_times > 1) ||
                tctx.soft_fails;
            bool show = test_global.show_output;

            if (has_msg && (of_interest || show)) {
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

    LOG_SITE(test_harness)->name = "test_harness";
    for (int i = 0; i < TEST_TIER_MAX; i++) {
        for (int j = 0; j < TEST_RESULT_MAX; j++) {
            test_global.results[i][j] += result_totals.totals[i][j];
            result_aggregates[j] += result_totals.totals[i][j];
        }
    }

    ndjson_emit(test_group_end, .group = tg->name, .duration_ms = total_time,
                .failed = result_aggregates[TEST_RESULT_FAILED],
                .skipped = result_aggregates[TEST_RESULT_SKIPPED]);

    if (!result_aggregates[TEST_RESULT_SKIPPED] &&
        !result_aggregates[TEST_RESULT_FAILED]) {
        test_harness_info("Test group " ANSI_BLUE ANSI_BOLD "%s" ANSI_RESET
                          " " ANSI_GREEN ANSI_BOLD "successful" ANSI_RESET
                          " in " ANSI_BOLD "%zu" ANSI_RESET " ms\n\n\n",
                          tg->name, total_time);
        LOG_SITE(test_harness)->name = "test_harness";
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

static bool sig_str_equal(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        while (*s1 == ' ' || *s1 == '\t')
            s1++;
        while (*s2 == ' ' || *s2 == '\t')
            s2++;
        if (*s1 != *s2)
            return false;
        if (*s1) {
            s1++;
            s2++;
        }
    }
    while (*s1 == ' ' || *s1 == '\t')
        s1++;
    while (*s2 == ' ' || *s2 == '\t')
        s2++;
    return *s1 == *s2;
}

void test_verify_signatures(void) {
    for (const struct test_signature_record *u =
             __skernel_test_unsafe_signatures;
         u < __ekernel_test_unsafe_signatures; u++) {

        for (const struct test_signature_record *c =
                 __skernel_test_canonical_signatures;
             c < __ekernel_test_canonical_signatures; c++) {

            if (strcmp(u->name, c->name) == 0) {
                bool ret_match = sig_str_equal(u->ret_str, c->ret_str);
                bool args_match = sig_str_equal(u->args_str, c->args_str);

                if (!ret_match || !args_match) {
                    test_harness_warn(
                        "Unsafe import '%s' signature mismatch "
                        "at %s:%u\n"
                        "  canonical: %s %s(%s) (defined at %s:%u)\n"
                        "  imported : %s %s(%s)\n",
                        u->name, u->file, u->line, c->ret_str, c->name,
                        c->args_str, c->file, c->line, u->ret_str, u->name,
                        u->args_str);
                }
                break;
            }
        }
    }
}

static void tests_resolve_imports(void) {
    for (const struct test_export_entry *a = __skernel_test_exports;
         a < __ekernel_test_exports; a++) {
        for (const struct test_export_entry *b = a + 1;
             b < __ekernel_test_exports; b++) {
            if (strcmp(a->name, b->name) == 0) {
                panic("duplicate '%s' exported, use TEST_EXPORT_AS", a->name);
            }
        }
    }

    for (const struct test_import_entry *imp = __skernel_test_imports;
         imp < __ekernel_test_imports; imp++) {
        void *resolved = NULL;

        for (const struct test_export_entry *exp = __skernel_test_exports;
             exp < __ekernel_test_exports; exp++) {
            if (strcmp(imp->name, exp->name) == 0) {
                resolved = exp->fn_ptr;
                break;
            }
        }

        if (!resolved) {
            panic("unresolved symbol '%s' imported at %s:%u!", imp->name,
                  imp->import_file, imp->import_line);
        }

        *(imp->target_fn_ptr) = resolved;
    }
}

static void tests_set_intensities() {
    for (struct test_group *tg = __skernel_test_groups;
         tg < __ekernel_test_groups; tg++) {
        if (tg->default_intensity == TEST_INTENSITY_SENTINEL)
            tg->default_intensity = TEST_INTENSITY_DEFAULT;

        if (test_global.global_intensity != TEST_INTENSITY_SENTINEL)
            tg->default_intensity = test_global.global_intensity;
    }

    for (struct test *t = __skernel_tests; t < __ekernel_tests; t++) {
        if (t->flags & TEST_FLAG_INHERITS_INTENSITY) {
            t->flags |= TEST_FLAG_HONORS_INTENSITY;
            if (t->intensity_desc.curve == SCALE_NONE) {
                t->intensity_desc.curve = t->group->intensity_desc.curve;
                t->intensity_desc.custom_scale =
                    t->group->intensity_desc.custom_scale;
                t->intensity_desc.custom_print =
                    t->group->intensity_desc.custom_print;

                if (t->intensity_desc.min_val == SIZE_MAX)
                    t->intensity_desc.min_val =
                        t->group->intensity_desc.min_val;

                if (t->intensity_desc.max_val == SIZE_MAX)
                    t->intensity_desc.max_val =
                        t->group->intensity_desc.max_val;

                if (t->intensity_desc.def_val == SIZE_MAX)
                    t->intensity_desc.def_val =
                        t->group->intensity_desc.def_val;
            }
        }

        if (t->intensity == TEST_INTENSITY_SENTINEL) {
            if (t->flags & TEST_FLAG_INHERITS_INTENSITY) {
                t->intensity = t->group->default_intensity;
            } else {
                t->intensity = TEST_INTENSITY_DEFAULT;
            }
        }

        if (test_global.global_intensity != TEST_INTENSITY_SENTINEL)
            t->intensity = test_global.global_intensity;
    }
}

void tests_run(void) {
#ifdef TEST_ENABLED
    term_probe();
    tests_resolve_imports();
    tests_check_duplicate_names();
    tests_setup_groups();
    tests_apply_filters();
    tests_set_intensities();
    tests_set_enabled_states();

    bool all_ok = true;
    char *msg = all_ok ? "all tests pass 🎉!" : "some errors occurred";
    char *color = all_ok ? ANSI_GREEN : ANSI_RED;

    test_harness_info("Running " ANSI_BOLD "%zu" ANSI_RESET " tests:\n",
                      test_global.total_tests_enabled);

    ndjson_emit(test_begin, .declared_total = test_global.total_tests_enabled);

    if (!test_global.no_progress) {
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

    ndjson_emit(test_totals, .total = test_global.total_tests_enabled,
                .passed = pass_count, .failed = fail_count,
                .skipped = skip_count);
    ndjson_emit(test_verdict, .ok = all_ok, .duration_ms = total_time);

    /* Give it the return code */
    if (!test_global.no_exit) {
        int code = all_ok ? TEST_EXIT_OK : TEST_EXIT_FAIL;
        ndjson_bye(code, all_ok ? "tests passed" : "tests failed");
        qemu_exit(code);
    }
#endif
}
