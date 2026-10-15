#include "internal.h"

#include <cmdline.h>
#include <global.h>
#include <nightmare/perturb.h>
#include <nightmare/record.h>
#include <sch/sched.h>
#include <string.h>
#include <test/perturb.h>
#include <thread/thread.h>
#include <time/time.h>

/* Scheduler rounds quiesce waits give before failing */
#define NIGHTMARE_QUIESCE_MIN_ROUNDS 8

static struct test_perturb_config *perturb_resolve_config(const char *path,
                                                          size_t path_len) {
    static const char prefix[] = "perturb.";
    if (path_len <= sizeof(prefix) - 1 ||
        strncmp(path, prefix, sizeof(prefix) - 1) != 0)
        return NULL;

    return test_perturb_config_lookup_n(path + sizeof(prefix) - 1,
                                        path_len - (sizeof(prefix) - 1));
}

static void *perturb_interval_resolve(const char *path, size_t path_len) {
    struct test_perturb_config *config = perturb_resolve_config(path, path_len);
    return config && strcmp(config->name, "stutter") != 0 ? config : NULL;
}

static void *perturb_stutter_resolve(const char *path, size_t path_len) {
    struct test_perturb_config *config = perturb_resolve_config(path, path_len);
    return config && strcmp(config->name, "stutter") == 0 ? config : NULL;
}

CMDLINE_SCHEMA_DEFINE(
    nightmare_perturb_interval, "nightmare", "perturb.<interval-svc>",
    "Nightmare interval perturbation options", perturb_interval_resolve,
    CMDLINE_SCHEMA_PROP(struct test_perturb_config, interval_us,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),
                        .range = RANGE(US_TO_NS(1), TIME_NS_MAX)));

CMDLINE_SCHEMA_DEFINE(
    nightmare_perturb_stutter, "nightmare", "perturb.stutter",
    "Nightmare stutter perturbation options", perturb_stutter_resolve,
    CMDLINE_SCHEMA_PROP(struct test_perturb_config, period_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),
                        .range = RANGE(MS_TO_NS(1), TIME_NS_MAX)),
    CMDLINE_SCHEMA_PROP(struct test_perturb_config, gap_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),
                        .range = RANGE(MS_TO_NS(1), TIME_NS_MAX)));

void nightmare_perturb_stutter(const struct test_perturb_env *env,
                               struct test_conc_worker *worker) {
    cc_ignore(env, worker);
    struct nightmare_ctx *ctx = &nightmare_runtime.ctx;
    const struct test_perturb_config *cfg =
        test_perturb_config_lookup("stutter");
    time_ms_t period_ms =
        (cfg && cfg->period_ms) ? NS_TO_MS(cfg->period_ms) : 100;
    time_ms_t gap_ms = (cfg && cfg->gap_ms) ? NS_TO_MS(cfg->gap_ms) : 5;

    while (!nightmare_must_stop()) {
        thread_sleep_for_ms(period_ms);
        if (nightmare_must_stop())
            break;

        /* Request quiesce */
        atomic_store_release(&nightmare_runtime.conc.quiesce_requested, true);

        /* Wait for subject workers to park, with signs of workers
         * parking preventing the panic from firing so that VMs
         * with oversubscribed vCPUs don't false positive */
        time_ms_t grace_ms = gap_ms ? gap_ms : 10;
        time_ms_t cap_ms = grace_ms * (ctx->worker_count + 1);
        time_ms_t started_ms = time_get_ms();
        time_ms_t last_park_ms = started_ms;
        size_t parked_seen = 0;
        size_t rounds = 0;
        bool all_subjects_parked = false;
        while (!nightmare_must_stop()) {
            size_t parked = 0;
            for (size_t i = 0; i < ctx->worker_count; i++) {
                if (atomic_load_acq(&nightmare_runtime.conc.workers[i].parked))
                    parked++;
            }
            all_subjects_parked = parked == ctx->worker_count;
            if (all_subjects_parked)
                break;

            time_ms_t now = time_get_ms();
            if (parked > parked_seen) {
                parked_seen = parked;
                last_park_ms = now;
            }

            /* Give a few rounds of leeway */
            if (rounds++ >= NIGHTMARE_QUIESCE_MIN_ROUNDS &&
                (now - last_park_ms >= grace_ms || now - started_ms >= cap_ms))
                break;

            scheduler_yield();
        }

        if (nightmare_must_stop()) {
            nightmare_record_quiesce(&(struct nightmare_quiesce_record){
                .result = "aborted",
                .checks = 0,
            });
            atomic_store_release(&nightmare_runtime.conc.quiesce_requested,
                                 false);
            break;
        }

        struct nightmare_verdict verdict = NIGHTMARE_OK;
        uint64_t checks = 0;
        if (!all_subjects_parked) {
            verdict = NIGHTMARE_FAIL(
                "quiesce_timeout",
                "not all subject workers parked before the stutter deadline");
        } else if (ctx->nm && ctx->nm->ops && ctx->nm->ops->quiesce_check) {
            verdict = ctx->nm->ops->quiesce_check(ctx);
            checks = 1;
        }

        nightmare_record_quiesce(&(struct nightmare_quiesce_record){
            .result = nightmare_result_to_str(verdict.result),
            .checks = checks,
        });

        if (verdict.result != NIGHTMARE_RESULT_OK)
            nightmare_publish_perturb_verdict(verdict);

        /* Release the herd */
        atomic_store_release(&nightmare_runtime.conc.quiesce_requested, false);

        if (verdict.result != NIGHTMARE_RESULT_OK) {
            nightmare_publish_stop(TEST_STOP_FAIL);
            break;
        }
    }
}

static const struct test_perturb_desc stutter_desc = {
    .name = "stutter",
    .fn = nightmare_perturb_stutter,
};

const struct test_perturb_desc *nightmare_perturb_lookup(const char *name) {
    if (name && strcmp(name, stutter_desc.name) == 0)
        return &stutter_desc;

    return test_perturb_lookup(name);
}
