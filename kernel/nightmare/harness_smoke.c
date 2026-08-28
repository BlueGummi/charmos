#include <asm.h>
#include <nightmare/nightmare.h>
#include <sch/sched.h>
#include <thread/thread.h>

struct harness_smoke_options {
    bool stall;
    bool plateau;
    bool blocked_drain;
    bool starve_one;
    time_ns_t park_delay_ms;
};

static struct harness_smoke_options harness_smoke_options;

NIGHTMARE_OPTIONS_DECLARE(
    harness_smoke, struct harness_smoke_options, harness_smoke_options,
    CMDLINE_SCHEMA_PROP(struct harness_smoke_options, stall),
    CMDLINE_SCHEMA_PROP(struct harness_smoke_options, plateau),
    CMDLINE_SCHEMA_PROP(struct harness_smoke_options, blocked_drain),
    CMDLINE_SCHEMA_PROP(struct harness_smoke_options, starve_one),
    CMDLINE_SCHEMA_PROP(struct harness_smoke_options, park_delay_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION)));

#ifdef TEST_NIGHTMARE_SMOKE
NIGHTMARE_WORKER(harness_smoke_worker) {
    if (harness_smoke_options.blocked_drain) {
        while (true)
            cpu_relax();
    }

    if (harness_smoke_options.plateau ||
        (harness_smoke_options.starve_one && NM_SELF->index == 0)) {
        while (!nightmare_must_stop())
            scheduler_yield();
        return;
    }

    if (harness_smoke_options.stall) {
        while (true) {
            NIGHTMARE_PROGRESS();
            cpu_relax();
        }
    }

    while (!nightmare_must_stop()) {
        if (nightmare_must_park()) {
            if (harness_smoke_options.park_delay_ms)
                thread_sleep_for_ms(
                    NS_TO_MS(harness_smoke_options.park_delay_ms));
            nightmare_park(NM_SELF);
        }
        NIGHTMARE_PROGRESS();
        scheduler_yield();
    }
}

static struct nightmare_verdict
harness_smoke_quiesce(struct nightmare_ctx *ctx) {
    (void) ctx;
    return NIGHTMARE_OK;
}

static const struct nightmare_ops harness_smoke_ops = {
    .worker = harness_smoke_worker,
    .quiesce_check = harness_smoke_quiesce,
};

NIGHTMARE_DECLARE(harness_smoke,
                  .desc = "Temporary P4 harness lifecycle smoke subject",
                  .ops = &harness_smoke_ops,
                  .seed_policy = NIGHTMARE_SEED_IGNORED,
                  NIGHTMARE_INTENSITY_CORES(1, 1, 1, "workers/core"),
                  .default_duration_ms = 1000);
#endif
