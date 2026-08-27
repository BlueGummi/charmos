#include "internal.h"

#include <cmdline.h>
#include <global.h>
#include <inject.h>
#include <mem/alloc_or_die.h>
#include <nightmare/perturb.h>
#include <nightmare/record.h>
#include <sch/sched.h>
#include <string.h>
#include <thread/apc.h>
#include <thread/thread.h>
#include <time/time.h>

static struct nightmare_perturb_config perturb_configs[] = {
    {.name = "migrator"}, {.name = "waker"},          {.name = "apc_spammer"},
    {.name = "stutter"},  {.name = "alloc_pressure"}, {.name = "inject_armer"},
};

static struct nightmare_perturb_config *
perturb_resolve_config(const char *path, size_t path_len) {
    static const char prefix[] = "perturb.";
    if (path_len <= sizeof(prefix) - 1 ||
        strncmp(path, prefix, sizeof(prefix) - 1) != 0)
        return NULL;

    path += sizeof(prefix) - 1;
    path_len -= sizeof(prefix) - 1;
    for (size_t i = 0; i < sizeof(perturb_configs) / sizeof(perturb_configs[0]);
         i++) {
        if (strlen(perturb_configs[i].name) == path_len &&
            strncmp(perturb_configs[i].name, path, path_len) == 0)
            return &perturb_configs[i];
    }
    return NULL;
}

static void *perturb_interval_resolve(const char *path, size_t path_len) {
    struct nightmare_perturb_config *config =
        perturb_resolve_config(path, path_len);
    return config && strcmp(config->name, "stutter") != 0 ? config : NULL;
}

static void *perturb_stutter_resolve(const char *path, size_t path_len) {
    struct nightmare_perturb_config *config =
        perturb_resolve_config(path, path_len);
    return config && strcmp(config->name, "stutter") == 0 ? config : NULL;
}

CMDLINE_SCHEMA_DECLARE(
    nightmare_perturb_interval, "nightmare", "perturb.<interval-svc>",
    "Nightmare interval perturbation options", perturb_interval_resolve,
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, interval_us,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),
                        .range = RANGE(US_TO_NS(1), TIME_NS_MAX)));

CMDLINE_SCHEMA_DECLARE(
    nightmare_perturb_stutter, "nightmare", "perturb.stutter",
    "Nightmare stutter perturbation options", perturb_stutter_resolve,
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, period_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),
                        .range = RANGE(MS_TO_NS(1), TIME_NS_MAX)),
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, gap_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),
                        .range = RANGE(MS_TO_NS(1), TIME_NS_MAX)));

const struct nightmare_perturb_config *
nightmare_perturb_config_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(perturb_configs) / sizeof(perturb_configs[0]);
         i++) {
        if (strcmp(perturb_configs[i].name, name) == 0)
            return &perturb_configs[i];
    }
    return NULL;
}

static inline void nightmare_perturb_delay(time_us_t interval_us) {
    thread_sleep_for_us(interval_us);
}

void nightmare_perturb_migrator(struct nightmare_ctx *ctx,
                                struct nightmare_worker *worker) {
    const struct nightmare_perturb_config *cfg =
        nightmare_perturb_config_lookup("migrator");
    time_us_t interval_us =
        (cfg && cfg->interval_us) ? NS_TO_US(cfg->interval_us) : 200;

    while (!nightmare_must_stop()) {
        if (nightmare_must_park()) {
            nightmare_park(worker);
            continue;
        }

        if (ctx->worker_count > 0 && global.core_count > 1) {
            size_t idx = nightmare_rand(&worker->rng) % ctx->worker_count;
            struct nightmare_worker *target_worker =
                &nightmare_runtime.workers[idx];
            struct thread *target_th =
                atomic_load_explicit(&target_worker->th, memory_order_acquire);
            if (target_th) {
                uint64_t target_cpu =
                    nightmare_rand(&worker->rng) % global.core_count;
                thread_set_migration_target(target_th, (int64_t) target_cpu);
            }
        }

        nightmare_perturb_delay(interval_us);
    }
}

void nightmare_perturb_waker(struct nightmare_ctx *ctx,
                             struct nightmare_worker *worker) {
    const struct nightmare_perturb_config *cfg =
        nightmare_perturb_config_lookup("waker");
    time_us_t interval_us =
        (cfg && cfg->interval_us) ? NS_TO_US(cfg->interval_us) : 250;

    while (!nightmare_must_stop()) {
        if (nightmare_must_park()) {
            nightmare_park(worker);
            continue;
        }

        if (ctx->worker_count > 0) {
            size_t idx = nightmare_rand(&worker->rng) % ctx->worker_count;
            struct nightmare_worker *target_worker =
                &nightmare_runtime.workers[idx];
            struct thread *target_th =
                atomic_load_explicit(&target_worker->th, memory_order_acquire);
            if (target_th) {
                thread_wake(target_th, THREAD_WAKE_REASON_SLEEP_MANUAL,
                            THREAD_PRIO_CLASS_TIMESHARE, NULL);
            }
        }

        nightmare_perturb_delay(interval_us);
    }
}

static void nightmare_apc_probe(void *arg) {
    (void) arg;
}

void nightmare_perturb_apc_spammer(struct nightmare_ctx *ctx,
                                   struct nightmare_worker *worker) {
    const struct nightmare_perturb_config *cfg =
        nightmare_perturb_config_lookup("apc_spammer");
    time_us_t interval_us =
        (cfg && cfg->interval_us) ? NS_TO_US(cfg->interval_us) : 300;

    while (!nightmare_must_stop()) {
        if (nightmare_must_park()) {
            nightmare_park(worker);
            continue;
        }

        if (ctx->worker_count > 0) {
            size_t idx = nightmare_rand(&worker->rng) % ctx->worker_count;
            struct nightmare_worker *target_worker =
                &nightmare_runtime.workers[idx];
            struct thread *target_th =
                atomic_load_explicit(&target_worker->th, memory_order_acquire);
            if (target_th && thread_get(target_th)) {
                struct apc *apc = apc_create();
                if (apc) {
                    apc_init(apc, nightmare_apc_probe, ctx, apc_destroy_free);
                    apc_enqueue(target_th, apc, APC_TYPE_KERNEL);
                    apc_put(apc);
                }
                thread_put(target_th);
            }
        }

        nightmare_perturb_delay(interval_us);
    }
}

void nightmare_perturb_stutter(struct nightmare_ctx *ctx,
                               struct nightmare_worker *worker) {
    (void) worker;
    const struct nightmare_perturb_config *cfg =
        nightmare_perturb_config_lookup("stutter");
    time_ms_t period_ms =
        (cfg && cfg->period_ms) ? NS_TO_MS(cfg->period_ms) : 100;
    time_ms_t gap_ms = (cfg && cfg->gap_ms) ? NS_TO_MS(cfg->gap_ms) : 5;

    while (!nightmare_must_stop()) {
        thread_sleep_for_ms(period_ms);
        if (nightmare_must_stop())
            break;

        /* Request quiesce */
        atomic_store_explicit(&nightmare_runtime.quiesce_requested, true,
                              memory_order_release);

        /* Wait for all subject workers to park. */
        time_ms_t deadline = time_get_ms() + (gap_ms ? gap_ms : 10);
        bool all_subjects_parked = false;
        while (time_get_ms() < deadline && !nightmare_must_stop()) {
            all_subjects_parked = true;
            for (size_t i = 0; i < ctx->worker_count; i++) {
                if (!atomic_load_explicit(&nightmare_runtime.workers[i].parked,
                                          memory_order_acquire)) {
                    all_subjects_parked = false;
                    break;
                }
            }
            if (all_subjects_parked)
                break;
            scheduler_yield();
        }

        if (nightmare_must_stop()) {
            nightmare_record_quiesce(&(struct nightmare_quiesce_record){
                .result = "aborted",
                .checks = 0,
            });
            atomic_store_explicit(&nightmare_runtime.quiesce_requested, false,
                                  memory_order_release);
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
            .result = nightmare_result_string(verdict.result),
            .checks = checks,
        });

        if (verdict.result != NIGHTMARE_RESULT_OK)
            nightmare_publish_perturb_verdict(verdict);

        /* Release the herd */
        atomic_store_explicit(&nightmare_runtime.quiesce_requested, false,
                              memory_order_release);

        if (verdict.result != NIGHTMARE_RESULT_OK) {
            nightmare_publish_stop(NM_STOP_FAIL);
            break;
        }
    }
}

#define ALLOC_PRESSURE_SLOTS 64

void nightmare_perturb_alloc_pressure(struct nightmare_ctx *ctx,
                                      struct nightmare_worker *worker) {
    (void) ctx;
    const struct nightmare_perturb_config *cfg =
        nightmare_perturb_config_lookup("alloc_pressure");
    time_us_t interval_us =
        (cfg && cfg->interval_us) ? NS_TO_US(cfg->interval_us) : 200;

    void *slots[ALLOC_PRESSURE_SLOTS] = {0};
    size_t slot_sizes[ALLOC_PRESSURE_SLOTS] = {0};
    uint32_t slot_patterns[ALLOC_PRESSURE_SLOTS] = {0};

    while (!nightmare_must_stop()) {
        if (nightmare_must_park()) {
            nightmare_park(worker);
            continue;
        }

        size_t slot = nightmare_rand(&worker->rng) % ALLOC_PRESSURE_SLOTS;
        if (slots[slot]) {
            uint32_t *p = slots[slot];
            size_t words = slot_sizes[slot] / sizeof(uint32_t);
            uint32_t expected = slot_patterns[slot];
            for (size_t w = 0; w < words; w++) {
                if (p[w] != (expected ^ (uint32_t) w)) {
                    NIGHTMARE_FINDING(
                        "alloc_corruption",
                        "heap memory corruption in alloc_pressure slot");
                    break;
                }
            }
            kfree(slots[slot]);
            slots[slot] = NULL;
            slot_sizes[slot] = 0;
            slot_patterns[slot] = 0;
        } else {
            size_t size = 16 + (nightmare_rand(&worker->rng) % 8176);
            uint32_t pat = (uint32_t) nightmare_rand(&worker->rng);
            void *ptr = kmalloc(size, ALLOC_FLAGS_ZERO);
            if (ptr) {
                uint32_t *p = ptr;
                size_t words = size / sizeof(uint32_t);
                for (size_t w = 0; w < words; w++)
                    p[w] = pat ^ (uint32_t) w;
                slots[slot] = ptr;
                slot_sizes[slot] = size;
                slot_patterns[slot] = pat;
            }
        }

        nightmare_perturb_delay(interval_us);
    }

    for (size_t i = 0; i < ALLOC_PRESSURE_SLOTS; i++) {
        if (slots[i]) {
            kfree(slots[i]);
            slots[i] = NULL;
        }
    }
}

void nightmare_perturb_inject_armer(struct nightmare_ctx *ctx,
                                    struct nightmare_worker *worker) {
    (void) ctx;
    const struct nightmare_perturb_config *cfg =
        nightmare_perturb_config_lookup("inject_armer");
    time_us_t interval_us =
        (cfg && cfg->interval_us) ? NS_TO_US(cfg->interval_us) : 500;

    size_t site_count = __ekernel_inject_sites - __skernel_inject_sites;
    if (site_count == 0) {
        while (!nightmare_must_stop()) {
            if (nightmare_must_park()) {
                nightmare_park(worker);
                continue;
            }
            thread_sleep_for_ms(50);
        }
        return;
    }

    while (!nightmare_must_stop()) {
        if (nightmare_must_park()) {
            nightmare_park(worker);
            continue;
        }

        size_t idx = nightmare_rand(&worker->rng) % site_count;
        struct inject_site *site = &__skernel_inject_sites[idx];
        uint32_t seed = (uint32_t) nightmare_rand(&worker->rng);
        uint32_t nth = 1 + (nightmare_rand(&worker->rng) % 10);

        inject_arm(site, seed, nth);
        nightmare_perturb_delay(interval_us);
        inject_disarm(site);
    }
}

static const struct nightmare_perturb_desc perturb_descs[] = {
    {.name = "migrator", .thread = nightmare_perturb_migrator},
    {.name = "waker", .thread = nightmare_perturb_waker},
    {.name = "apc_spammer", .thread = nightmare_perturb_apc_spammer},
    {.name = "stutter", .thread = nightmare_perturb_stutter},
    {.name = "alloc_pressure", .thread = nightmare_perturb_alloc_pressure},
    {.name = "inject_armer", .thread = nightmare_perturb_inject_armer},
};

const struct nightmare_perturb_desc *
nightmare_perturb_lookup(const char *name) {
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(perturb_descs) / sizeof(perturb_descs[0]);
         i++) {
        if (strcmp(perturb_descs[i].name, name) == 0)
            return &perturb_descs[i];
    }
    return NULL;
}
