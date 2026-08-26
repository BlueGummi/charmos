/* @title: Nightmare perturbation */
#pragma once
#include <math/fixed.h>
#include <stdbool.h>
#include <types/types.h>

struct nightmare_ctx;
struct nightmare_worker;

struct nightmare_perturb_config {
    const char *name;
    bool enabled;
    time_ns_t interval_us;
    time_ns_t period_ms;
    time_ns_t gap_ms;
    fx32_32_t aggression;
};

struct nightmare_perturb_desc {
    const char *name;
    void (*thread)(struct nightmare_ctx *, struct nightmare_worker *);
};

const struct nightmare_perturb_config *
nightmare_perturb_config_lookup(const char *name);
const struct nightmare_perturb_desc *nightmare_perturb_lookup(const char *name);
