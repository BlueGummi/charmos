#include "internal.h"

#include <asm.h>
#include <console/panic.h>
#include <console/printf.h>
#include <global.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <ndjson.h>
#include <nightmare/perturb.h>
#include <nightmare/record.h>
#include <smp/smp.h>
#include <string.h>
#include <thread/thread.h>
#include <time/time.h>

#ifndef CHARMOS_COMMIT
#define CHARMOS_COMMIT "unknown"
#endif

#define NIGHTMARE_MIB (1024ULL * 1024ULL)
#define NIGHTMARE_DEFAULT_DURATION_MS 1000
#define NIGHTMARE_DEFAULT_DRAIN_MS 20000
#define NIGHTMARE_DEFAULT_STAT_MS 5000

struct nightmare_runtime nightmare_runtime;

static void nightmare_exit(uint8_t code, const char *reason) {
    ndjson_bye(code, reason);
    qemu_exit(code);
    while (true)
        cpu_relax();
}

const char *nightmare_result_string(enum nightmare_result result) {
    switch (result) {
    case NIGHTMARE_RESULT_OK: return "ok";
    case NIGHTMARE_RESULT_FINDING: return "finding";
    case NIGHTMARE_RESULT_FAIL: return "fail";
    case NIGHTMARE_RESULT_STALL: return "stall";
    case NIGHTMARE_RESULT_SKIP: return "skip";
    default: return "unknown";
    }
}

const char *nightmare_skip_string(enum nightmare_skip_reason reason) {
    switch (reason) {
    case NIGHTMARE_SKIP_NONE: return "none";
    case NIGHTMARE_SKIP_NOT_COMPILED: return "not_compiled";
    case NIGHTMARE_SKIP_NO_SUCH_NIGHTMARE: return "no_such_nightmare";
    case NIGHTMARE_SKIP_NEEDS_SMP: return "needs_smp";
    case NIGHTMARE_SKIP_NEEDS_PREEMPT: return "needs_preempt";
    case NIGHTMARE_SKIP_NEEDS_ASAN: return "needs_asan";
    case NIGHTMARE_SKIP_NEEDS_INJECT: return "needs_inject";
    case NIGHTMARE_SKIP_RAM_LOW: return "ram_low";
    case NIGHTMARE_SKIP_SEED_UNUSED: return "seed_unused";
    case NIGHTMARE_SKIP_SEED_MISSING: return "seed_missing";
    case NIGHTMARE_SKIP_SERVICE_MISSING: return "service_missing";
    case NIGHTMARE_SKIP_PREPARE_REFUSED: return "prepare_refused";
    default: return "unknown";
    }
}

const char *nightmare_seed_policy_string(enum nightmare_seed_policy policy) {
    switch (policy) {
    case NIGHTMARE_SEED_IGNORED: return "ignored";
    case NIGHTMARE_SEED_OPTIONAL: return "optional";
    case NIGHTMARE_SEED_REQUIRED: return "required";
    default: return "unknown";
    }
}

const char *nightmare_seed_mode_string(enum nightmare_seed_mode mode) {
    switch (mode) {
    case NIGHTMARE_SEED_SPLIT: return "split";
    case NIGHTMARE_SEED_SEEDFUL: return "seedful";
    case NIGHTMARE_SEED_SEEDLESS: return "seedless";
    default: return "unknown";
    }
}

static uint8_t nightmare_exit_for_result(enum nightmare_result result) {
    switch (result) {
    case NIGHTMARE_RESULT_OK: return NIGHTMARE_EXIT_OK;
    case NIGHTMARE_RESULT_FINDING: return NIGHTMARE_EXIT_FINDING;
    case NIGHTMARE_RESULT_FAIL: return NIGHTMARE_EXIT_FAIL;
    case NIGHTMARE_RESULT_STALL: return NIGHTMARE_EXIT_STALL;
    case NIGHTMARE_RESULT_SKIP: return NIGHTMARE_EXIT_SKIP;
    default: return NIGHTMARE_EXIT_FAIL;
    }
}

static uint64_t nightmare_progress_snapshot(void) {
#ifdef TEST_NIGHTMARE_ENABLED
    return nightmare_progress_sum_irq();
#else
    return 0;
#endif
}

static void append_cap(char *caps, size_t cap, const char *value) {
    size_t used = strlen(caps);
    if (used >= cap - 1)
        return;
    snprintf(caps + used, (int) (cap - used), "%s%s", used ? "," : "", value);
}

static void nightmare_build_caps(void) {
    nightmare_runtime.caps[0] = '\0';
    append_cap(nightmare_runtime.caps, sizeof(nightmare_runtime.caps),
               "preempt");
#ifdef DEBUG_ASAN
    append_cap(nightmare_runtime.caps, sizeof(nightmare_runtime.caps), "asan");
#endif
#ifdef INJECT_ENABLED
    append_cap(nightmare_runtime.caps, sizeof(nightmare_runtime.caps),
               "inject");
#endif
}

#ifdef TEST_NIGHTMARE_ENABLED
static const struct nightmare *nightmare_resolve(const char *name) {
    const struct nightmare *match = NULL;
    for (struct nightmare *nm = __skernel_nightmares; nm < __ekernel_nightmares;
         nm++) {
        if (!nm->name)
            nightmare_panic("nightmare registry contains an unnamed entry");
        if (strcmp(nm->name, name) != 0)
            continue;
        if (match)
            nightmare_panic("duplicate nightmare name '%s' in %s and %s", name,
                            match->fname, nm->fname);
        match = nm;
    }
    return match;
}
#endif

static void
nightmare_record_resolved_boot(const struct nightmare_cmdline_config *config,
                               const char *requested) {
    const struct nightmare *nm = nightmare_runtime.ctx.nm;
    nightmare_record_boot(&(struct nightmare_boot_record){
        .test = nm ? nm->name : requested,
        .seed = config->seed,
        .seed_policy =
            nm ? nightmare_seed_policy_string(nm->seed_policy) : "unknown",
        .seed_mode = nightmare_seed_mode_string(config->seed_mode),
        .intensity = nightmare_runtime.ctx.intensity,
        .intensity_val = nightmare_runtime.ctx.intensity_val,
        .workers = nightmare_runtime.ctx.worker_count,
        .smp = global.core_count,
        .mem_mib = pmm_get_usable_ram() / NIGHTMARE_MIB,
        .caps = nightmare_runtime.caps,
        .campaign_id = config->campaign_id ? config->campaign_id : "",
        .boot_index = config->boot_index,
        .commit = CHARMOS_COMMIT,
    });
}

static void nightmare_emit_verdict(struct nightmare_verdict verdict,
                                   const char *fallback_reason) {
    enum nightmare_result result = verdict.result;
    size_t findings = atomic_load_explicit(&nightmare_runtime.finding_count,
                                           memory_order_acquire);
    if (result == NIGHTMARE_RESULT_OK && findings)
        result = NIGHTMARE_RESULT_FINDING;

    const char *reason = verdict.reason;
    if (result == NIGHTMARE_RESULT_SKIP)
        reason = nightmare_skip_string(verdict.skip_reason);
    if (!reason)
        reason = fallback_reason;

    time_ms_t now = time_get_ms();
    uint64_t elapsed =
        nightmare_runtime.started_ms ? now - nightmare_runtime.started_ms : 0;
    nightmare_record_verdict(&(struct nightmare_verdict_record){
        .result = nightmare_result_string(result),
        .reason = reason ? reason : "unknown",
        .duration_ms = elapsed,
        .progress = nightmare_progress_snapshot(),
        .findings = findings,
        .msg = verdict.msg ? verdict.msg : "",
    });
    nightmare_exit(nightmare_exit_for_result(result),
                   reason ? reason : nightmare_result_string(result));
}

#ifdef TEST_NIGHTMARE_ENABLED
static enum nightmare_skip_reason
nightmare_seed_refusal(const struct nightmare *nm,
                       const struct nightmare_cmdline_config *config) {
    if (config->seed_mode != NIGHTMARE_SEED_SEEDLESS && !config->seed_present)
        return NIGHTMARE_SKIP_SEED_MISSING;
    if (nm->seed_policy == NIGHTMARE_SEED_IGNORED &&
        config->seed_mode == NIGHTMARE_SEED_SEEDFUL)
        return NIGHTMARE_SKIP_SEED_UNUSED;
    if (nm->seed_policy == NIGHTMARE_SEED_REQUIRED &&
        config->seed_mode != NIGHTMARE_SEED_SEEDFUL)
        return NIGHTMARE_SKIP_SEED_MISSING;
    return NIGHTMARE_SKIP_NONE;
}

static bool
nightmare_has_missing_service(const struct nightmare *nm,
                              const struct nightmare_cmdline_config *config) {
    if (nm->perturb) {
        for (const char *const *name = nm->perturb; *name; name++) {
            if (!nightmare_perturb_lookup(*name))
                return true;
        }
    }

    if (!config->perturb_present)
        return false;
    for (size_t i = 0; i < config->perturb.count; i++) {
        const char *name = NULL;
        if (cmdline_extract_const_string(&config->perturb.items[i], &name) !=
                ERR_OK ||
            !nightmare_perturb_lookup(name))
            return true;
    }
    return false;
}

static enum nightmare_skip_reason
nightmare_preflight(const struct nightmare *nm,
                    const struct nightmare_cmdline_config *config) {
    if ((nm->requires & NIGHTMARE_REQ_SMP) && global.core_count < 2)
        return NIGHTMARE_SKIP_NEEDS_SMP;
    /* Today, we always have preemption enabled, however TODO: someday
     * we can implement support for a non-preemptive kernel*/
    if ((nm->requires & NIGHTMARE_REQ_PREEMPT) == 0) {
        /* No-op */
    }
#ifndef DEBUG_ASAN
    if (nm->requires & NIGHTMARE_REQ_ASAN)
        return NIGHTMARE_SKIP_NEEDS_ASAN;
#endif
#ifndef INJECT_ENABLED
    if (nm->requires & NIGHTMARE_REQ_INJECT)
        return NIGHTMARE_SKIP_NEEDS_INJECT;
#endif
    if (pmm_get_usable_ram() / NIGHTMARE_MIB < nm->min_mem_mib)
        return NIGHTMARE_SKIP_RAM_LOW;

    enum nightmare_skip_reason seed = nightmare_seed_refusal(nm, config);
    if (seed != NIGHTMARE_SKIP_NONE)
        return seed;
    if (nightmare_has_missing_service(nm, config))
        return NIGHTMARE_SKIP_SERVICE_MISSING;
    return NIGHTMARE_SKIP_NONE;
}

static void nightmare_soft_deadline(struct timer *timer) {
    (void) timer;
    nightmare_publish_stop(NM_STOP_BUDGET);
}

static void nightmare_hard_deadline(struct timer *timer) {
    (void) timer;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &nightmare_runtime.terminal, &expected, true, memory_order_acq_rel,
            memory_order_acquire))
        return;

    nightmare_publish_stop(NM_STOP_STALL);
    ndjson_enter_panic();
    nightmare_record_verdict(&(struct nightmare_verdict_record){
        .result = "stall",
        .reason = "drain_timeout",
        .duration_ms = time_get_ms() - nightmare_runtime.started_ms,
        .progress = nightmare_progress_sum_irq(),
        .findings = atomic_load_explicit(&nightmare_runtime.finding_count,
                                         memory_order_relaxed),
        .msg = "hard deadline expired before teardown completed",
    });
    nightmare_exit(NIGHTMARE_EXIT_STALL, "hard deadline");
}

static void nightmare_arm_deadlines(time_ms_t duration_ms,
                                    time_ms_t drain_grace_ms) {
    time_us_t now = time_get_us();
    timer_init(&nightmare_runtime.soft_timer, nightmare_soft_deadline, NULL);
    nightmare_runtime.soft_timer.flags = TIMER_FLAG_IRQ;
    nightmare_runtime.soft_timer.expiration_us = now + MS_TO_US(duration_ms);
    timer_add_global(&nightmare_runtime.soft_timer);

    timer_init(&nightmare_runtime.hard_timer, nightmare_hard_deadline, NULL);
    nightmare_runtime.hard_timer.flags = TIMER_FLAG_IRQ;
    nightmare_runtime.hard_timer.expiration_us =
        now + MS_TO_US(duration_ms + drain_grace_ms);
    timer_add_global(&nightmare_runtime.hard_timer);
}

static uint64_t nightmare_worker_seed(const struct nightmare_ctx *ctx,
                                      size_t index) {
    uint64_t base =
        ctx->seed_mode == NIGHTMARE_SEED_SEEDFUL ? ctx->seed : time_get_ns();
    base ^= UINT64_C(0x9e3779b97f4a7c15) * (index + 1);
    struct nightmare_rng rng = {.state = base};
    return nightmare_rand(&rng);
}

static bool nightmare_spawn_threads(void) {
    size_t created = 0;
    for (size_t i = 0; i < nightmare_runtime.total_worker_count; i++) {
        struct nightmare_worker *worker = &nightmare_runtime.workers[i];
        worker->index = i;
        if (i < nightmare_runtime.ctx.worker_count) {
            worker->role = "worker";
        } else {
            size_t pidx = i - nightmare_runtime.ctx.worker_count;
            worker->role = nightmare_runtime.perturbers[pidx]->name;
        }
        worker->rng.state =
            nightmare_worker_seed(&nightmare_runtime.ctx, worker->index);
        struct thread *thread = thread_spawn_joinable(
            "nightmare_%s_%zu", nightmare_thread_main, worker, worker->role, i);
        if (!thread)
            goto fail;
        atomic_store_explicit(&worker->th, thread, memory_order_release);
        created++;
    }

    nightmare_runtime.heartbeat = thread_spawn_joinable(
        "nightmare_heartbeat", nightmare_heartbeat_main, NULL);
    if (!nightmare_runtime.heartbeat)
        goto fail;
    return true;

fail:
    nightmare_publish_stop(NM_STOP_FAIL);
    complete_all(&nightmare_runtime.start);
    for (size_t i = 0; i < created; i++) {
        struct thread *th = atomic_load_explicit(
            &nightmare_runtime.workers[i].th, memory_order_acquire);
        if (th)
            thread_join(th);
    }
    return false;
}

static void nightmare_join_threads(void) {
    for (size_t i = 0; i < nightmare_runtime.total_worker_count; i++) {
        struct thread *thread = atomic_load_explicit(
            &nightmare_runtime.workers[i].th, memory_order_acquire);
        if (thread)
            thread_join(thread);
    }
    if (nightmare_runtime.heartbeat)
        thread_join(nightmare_runtime.heartbeat);
}
static void nightmare_add_perturber(const struct nightmare_perturb_desc *desc) {
    if (!desc || nightmare_runtime.perturber_count >= NIGHTMARE_MAX_PERTURBERS)
        return;
    for (size_t i = 0; i < nightmare_runtime.perturber_count; i++) {
        if (nightmare_runtime.perturbers[i] == desc)
            return;
    }
    nightmare_runtime.perturbers[nightmare_runtime.perturber_count++] = desc;
}

static void
nightmare_collect_perturbers(const struct nightmare *nm,
                             const struct nightmare_cmdline_config *config) {
    nightmare_runtime.perturber_count = 0;
    if (nm->perturb) {
        for (const char *const *name = nm->perturb; *name; name++)
            nightmare_add_perturber(nightmare_perturb_lookup(*name));
    }
    if (config->perturb_present) {
        for (size_t i = 0; i < config->perturb.count; i++) {
            const char *name = NULL;
            if (cmdline_extract_const_string(&config->perturb.items[i],
                                             &name) == ERR_OK &&
                name)
                nightmare_add_perturber(nightmare_perturb_lookup(name));
        }
    }
}

static struct nightmare_verdict
nightmare_finalize_verdict(const struct nightmare *nm) {
    struct nightmare_verdict final = NIGHTMARE_OK;
    if (nm->ops && nm->ops->quiesce_check) {
        final = nm->ops->quiesce_check(&nightmare_runtime.ctx);
        nightmare_record_quiesce(&(struct nightmare_quiesce_record){
            .result = nightmare_result_string(final.result),
            .checks = 1,
        });
    }
    if (final.result == NIGHTMARE_RESULT_OK && nm->ops && nm->ops->finish)
        final = nm->ops->finish(&nightmare_runtime.ctx);

    enum nightmare_stop stop =
        atomic_load_explicit(&nightmare_runtime.stop, memory_order_acquire);
    if (stop == NM_STOP_STALL)
        final = (struct nightmare_verdict){
            .result = NIGHTMARE_RESULT_STALL,
            .reason = "liveness",
        };
    else if (stop == NM_STOP_FAIL && final.result == NIGHTMARE_RESULT_OK)
        final = NIGHTMARE_FAIL("harness", "harness requested termination");

    return final;
}
#endif

void nightmare_run(void) {
    struct nightmare_cmdline_config config;
    nightmare_cmdline_get(&config);
    if (!config.selector)
        return;

    nightmare_runtime = (struct nightmare_runtime){
        .ctx = {.seed = config.seed,
                .seed_present = config.seed_present,
                .seed_mode = config.seed_mode},
        .active = ATOMIC_VAR_INIT(false),
        .stop = ATOMIC_VAR_INIT(NM_RUN),
        .quiesce_requested = ATOMIC_VAR_INIT(false),
        .parked_count = ATOMIC_VAR_INIT(0),
        .finding_count = ATOMIC_VAR_INIT(0),
        .terminal = ATOMIC_VAR_INIT(false),
        .stat_interval_ms = config.stat_interval_ms ? config.stat_interval_ms
                                                    : NIGHTMARE_DEFAULT_STAT_MS,
        .campaign_id = config.campaign_id,
        .boot_index = config.boot_index,
    };
    completion_init(&nightmare_runtime.start, COMPLETION_INIT_NORMAL);
    nightmare_runtime.started_ms = time_get_ms();
    nightmare_build_caps();

#ifndef TEST_NIGHTMARE_ENABLED
    nightmare_record_resolved_boot(&config, config.selector);
    nightmare_emit_verdict(NIGHTMARE_SKIP(NIGHTMARE_SKIP_NOT_COMPILED),
                           "not_compiled");
#else
    const struct nightmare *nm = nightmare_resolve(config.selector);
    nightmare_runtime.ctx.nm = nm;
    if (!nm) {
        nightmare_record_resolved_boot(&config, config.selector);
        nightmare_emit_verdict(NIGHTMARE_SKIP(NIGHTMARE_SKIP_NO_SUCH_NIGHTMARE),
                               "no_such_nightmare");
    }

    nightmare_runtime.ctx.intensity =
        config.intensity != NIGHTMARE_INTENSITY_SENTINEL
            ? config.intensity
            : (nm->intensity != NIGHTMARE_INTENSITY_SENTINEL
                   ? nm->intensity
                   : NIGHTMARE_INTENSITY_DEFAULT);
    nightmare_runtime.ctx.intensity_val =
        scaled_param_eval(&nm->intensity_desc, nightmare_runtime.ctx.intensity);
    nightmare_runtime.ctx.worker_count =
        nightmare_runtime.ctx.intensity_val
            ? nightmare_runtime.ctx.intensity_val
            : 1;

    time_ms_t duration_ms =
        config.duration_ms
            ? config.duration_ms
            : (nm->default_duration_ms ? nm->default_duration_ms
                                       : NIGHTMARE_DEFAULT_DURATION_MS);
    time_ms_t drain_ms = config.drain_grace_ms ? config.drain_grace_ms
                                               : NIGHTMARE_DEFAULT_DRAIN_MS;
    nightmare_runtime.ctx.soft_deadline_ms =
        nightmare_runtime.started_ms + duration_ms;
    nightmare_runtime.ctx.hard_deadline_ms =
        nightmare_runtime.ctx.soft_deadline_ms + drain_ms;

    nightmare_record_resolved_boot(&config, config.selector);

    enum nightmare_skip_reason refusal = nightmare_preflight(nm, &config);
    if (refusal != NIGHTMARE_SKIP_NONE)
        nightmare_emit_verdict(NIGHTMARE_SKIP(refusal),
                               nightmare_skip_string(refusal));

    atomic_store_explicit(&nightmare_runtime.active, true,
                          memory_order_release);
    nightmare_arm_deadlines(duration_ms, drain_ms);

    struct nightmare_verdict prepared = NIGHTMARE_OK;
    if (nm->ops && nm->ops->prepare)
        prepared = nm->ops->prepare(&nightmare_runtime.ctx);
    if (prepared.result != NIGHTMARE_RESULT_OK) {
        timer_shutdown_sync(&nightmare_runtime.soft_timer);
        timer_shutdown_sync(&nightmare_runtime.hard_timer);
        if (prepared.result == NIGHTMARE_RESULT_SKIP)
            prepared = NIGHTMARE_SKIP(NIGHTMARE_SKIP_PREPARE_REFUSED);
        nightmare_emit_verdict(prepared, "prepare");
    }

    nightmare_collect_perturbers(nm, &config);

    nightmare_runtime.total_worker_count =
        nightmare_runtime.ctx.worker_count + nightmare_runtime.perturber_count;

    nightmare_runtime.workers = kmalloc(nightmare_runtime.total_worker_count *
                                            sizeof(*nightmare_runtime.workers),
                                        ALLOC_FLAGS_ZERO);
    if (!nightmare_runtime.workers) {
        timer_shutdown_sync(&nightmare_runtime.soft_timer);
        timer_shutdown_sync(&nightmare_runtime.hard_timer);
        nightmare_emit_verdict(
            NIGHTMARE_FAIL("worker_alloc", "could not allocate worker state"),
            "worker_alloc");
    }

    if (!nightmare_spawn_threads()) {
        timer_shutdown_sync(&nightmare_runtime.soft_timer);
        timer_shutdown_sync(&nightmare_runtime.hard_timer);
        nightmare_emit_verdict(
            NIGHTMARE_FAIL("thread_create", "could not create all threads"),
            "thread_create");
    }

    complete_all(&nightmare_runtime.start);
    nightmare_join_threads();
    nightmare_publish_stop(NM_STOP_BUDGET);

    timer_shutdown_sync(&nightmare_runtime.soft_timer);
    timer_shutdown_sync(&nightmare_runtime.hard_timer);

    struct nightmare_verdict final = nightmare_finalize_verdict(nm);
    atomic_store_explicit(&nightmare_runtime.active, false,
                          memory_order_release);

    kfree(nightmare_runtime.workers);
    nightmare_runtime.workers = NULL;
    if (nightmare_runtime.ctx.private) {
        kfree(nightmare_runtime.ctx.private);
        nightmare_runtime.ctx.private = NULL;
    }

    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(
            &nightmare_runtime.terminal, &expected, true, memory_order_acq_rel,
            memory_order_acquire))
        nightmare_emit_verdict(final, "completed");
#endif
}
