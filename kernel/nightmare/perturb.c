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
    {.name = "migrator"},     {.name = "waker"},   {.name = "apc_spammer"},
    {.name = "idle_forcer"},  {.name = "stutter"}, {.name = "alloc_pressure"},
    {.name = "inject_armer"},
};

static void *perturb_resolve(const char *path, size_t path_len) {
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

CMDLINE_SCHEMA_DECLARE(
    nightmare_perturb, "nightmare", "perturb.<svc>",
    "Nightmare perturbation options", perturb_resolve,
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, enabled),
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, interval_us,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION)),
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, period_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION)),
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, gap_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION)),
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, aggression,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_FX),
                        .range = RANGE(0, FX_ONE)));

const struct nightmare_perturb_config *
nightmare_perturb_config_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(perturb_configs) / sizeof(perturb_configs[0]);
         i++) {
        if (strcmp(perturb_configs[i].name, name) == 0)
            return &perturb_configs[i];
    }
    return NULL;
}

static inline void nightmare_perturb_delay(time_ns_t interval_us) {
    if (interval_us >= 1000)
        thread_sleep_for_ms(interval_us / 1000);
    else
        scheduler_yield();
}

void nightmare_perturb_migrator(struct nightmare_ctx *ctx,
                                struct nightmare_worker *worker) {
    const struct nightmare_perturb_config *cfg =
        nightmare_perturb_config_lookup("migrator");
    time_ns_t interval_us = (cfg && cfg->interval_us) ? cfg->interval_us : 200;

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
    time_ns_t interval_us = (cfg && cfg->interval_us) ? cfg->interval_us : 250;

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
    time_ns_t interval_us = (cfg && cfg->interval_us) ? cfg->interval_us : 300;

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
                struct apc *apc = apc_create();
                if (apc) {
                    apc_init(apc, nightmare_apc_probe, ctx);
                    apc_enqueue(target_th, apc, APC_TYPE_KERNEL);
                }
            }
        }

        nightmare_perturb_delay(interval_us);
    }
}

void nightmare_perturb_idle_forcer(struct nightmare_ctx *ctx,
                                   struct nightmare_worker *worker) {
    const struct nightmare_perturb_config *cfg =
        nightmare_perturb_config_lookup("idle_forcer");
    time_ns_t interval_us = (cfg && cfg->interval_us) ? cfg->interval_us : 500;

    while (!nightmare_must_stop()) {
        if (nightmare_must_park()) {
            nightmare_park(worker);
            continue;
        }

        if (ctx->worker_count > 0 && global.core_count > 1) {
            uint64_t target_cpu =
                nightmare_rand(&worker->rng) % global.core_count;
            uint64_t dest_cpu = (target_cpu + 1) % global.core_count;

            for (size_t i = 0; i < ctx->worker_count; i++) {
                struct nightmare_worker *w = &nightmare_runtime.workers[i];
                struct thread *t =
                    atomic_load_explicit(&w->th, memory_order_acquire);
                if (t) {
                    thread_set_migration_target(t, (int64_t) dest_cpu);
                }
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
    time_ns_t period_ms = (cfg && cfg->period_ms) ? cfg->period_ms : 100;
    time_ns_t gap_ms = (cfg && cfg->gap_ms) ? cfg->gap_ms : 5;

    while (!nightmare_must_stop()) {
        thread_sleep_for_ms(period_ms);
        if (nightmare_must_stop())
            break;

        /* Request quiesce */
        atomic_store_explicit(&nightmare_runtime.quiesce_requested, true,
                              memory_order_release);

        /* Wait for all workers to park */
        time_ms_t deadline = time_get_ms() + (gap_ms ? gap_ms : 10);
        while (time_get_ms() < deadline && !nightmare_must_stop()) {
            size_t parked = atomic_load_explicit(
                &nightmare_runtime.parked_count, memory_order_acquire);
            if (parked >= ctx->worker_count)
                break;
            scheduler_yield();
        }

        /* Verify quiesce invariants if provided */
        if (ctx->nm && ctx->nm->ops && ctx->nm->ops->quiesce_check)
            ctx->nm->ops->quiesce_check(ctx);

        /* Release the herd */
        atomic_store_explicit(&nightmare_runtime.quiesce_requested, false,
                              memory_order_release);
    }
}

#define ALLOC_PRESSURE_SLOTS 64

void nightmare_perturb_alloc_pressure(struct nightmare_ctx *ctx,
                                      struct nightmare_worker *worker) {
    (void) ctx;
    const struct nightmare_perturb_config *cfg =
        nightmare_perturb_config_lookup("alloc_pressure");
    time_ns_t interval_us = (cfg && cfg->interval_us) ? cfg->interval_us : 200;

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
    time_ns_t interval_us = (cfg && cfg->interval_us) ? cfg->interval_us : 500;

    size_t site_count = __ekernel_inject_sites - __skernel_inject_sites;
    if (site_count == 0) {
        while (!nightmare_must_stop())
            thread_sleep_for_ms(50);
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
    {.name = "idle_forcer", .thread = nightmare_perturb_idle_forcer},
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
