/* @title: Nightmare perturbation */
#pragma once
#include <types/types.h>

struct nightmare_ctx;
struct nightmare_worker;

struct nightmare_perturb_config {
    const char *name;
    time_ns_t interval_us;
    time_ns_t period_ms;
    time_ns_t gap_ms;
};

struct nightmare_perturb_desc {
    const char *name;
    void (*thread)(struct nightmare_ctx *, struct nightmare_worker *);
};

const struct nightmare_perturb_config *
nightmare_perturb_config_lookup(const char *name);
const struct nightmare_perturb_desc *nightmare_perturb_lookup(const char *name);

void nightmare_perturb_migrator(struct nightmare_ctx *ctx,
                                struct nightmare_worker *worker);
void nightmare_perturb_waker(struct nightmare_ctx *ctx,
                             struct nightmare_worker *worker);
void nightmare_perturb_apc_spammer(struct nightmare_ctx *ctx,
                                   struct nightmare_worker *worker);
void nightmare_perturb_stutter(struct nightmare_ctx *ctx,
                               struct nightmare_worker *worker);
void nightmare_perturb_alloc_pressure(struct nightmare_ctx *ctx,
                                      struct nightmare_worker *worker);
void nightmare_perturb_inject_armer(struct nightmare_ctx *ctx,
                                    struct nightmare_worker *worker);
