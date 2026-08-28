#include "internal.h"

#include <cmdline.h>
#include <math/range.h>
#include <string.h>
#include <time/time.h>

/* This is a necessary evil, as the command line will
 * pass in nanoseconds, and we map to milliseconds in the config
 *
 * It also helps us expand the command line options without
 * bloating up a config file, if said options are operated
 * upon to arrive at the final value in the config */
static const char *nightmare_selector;
static fx32_32_t nightmare_intensity = NIGHTMARE_INTENSITY_SENTINEL;
static uint64_t nightmare_seed;
static enum nightmare_seed_mode nightmare_seed_mode = NIGHTMARE_SEED_SPLIT;
static time_ns_t nightmare_duration_ns;
static time_ns_t nightmare_drain_grace_ns = SECONDS_TO_NS(20);
static time_ns_t nightmare_stat_interval_ns = SECONDS_TO_NS(5);
static time_ns_t nightmare_stall_threshold_ns = MS_TO_NS(3000);
static enum nightmare_on_stall nightmare_on_stall = NIGHTMARE_ON_STALL_REPORT;
static uint64_t nightmare_boot_index;
static const char *nightmare_campaign_id;

CMDLINE_DECLARE_VAR(
    nightmare_root, nightmare_selector, .name = "nightmare",
    .types = CMDLINE_TYPES(CMDLINE_TYPE_STRING),
    .desc = "Select the single nightmare subject for this boot");

CMDLINE_CHILDREN_DECLARE(
    nightmare_root,
    CMDLINE_INNER_FX(intensity, nightmare_intensity,
                     .desc = "Nightmare intensity in [0,1]",
                     .range = RANGE(0, FX_ONE)),
    CMDLINE_INNER_VAR(seed, nightmare_seed, .desc = "Nightmare seed"),
    CMDLINE_INNER_VAR(seed_mode, nightmare_seed_mode,
                      .desc = "Seed application mode",
                      .mappings = CMDLINE_MAPPINGS(
                          CMDLINE_MAP("split", NIGHTMARE_SEED_SPLIT),
                          CMDLINE_MAP("seedful", NIGHTMARE_SEED_SEEDFUL),
                          CMDLINE_MAP("seedless", NIGHTMARE_SEED_SEEDLESS))),
    CMDLINE_INNER_DURATION(duration_ms, nightmare_duration_ns,
                           .desc = "Soft runtime budget"),
    CMDLINE_INNER_DURATION(drain_grace_ms, nightmare_drain_grace_ns,
                           .desc = "Soft-to-hard deadline grace"),
    CMDLINE_INNER_DURATION(stat_interval_ms, nightmare_stat_interval_ns,
                           .desc = "Heartbeat record cadence"),
    CMDLINE_INNER_DURATION(stall_threshold_ms, nightmare_stall_threshold_ns,
                           .desc = "Liveness stall threshold duration",
                           .range = RANGE(MS_TO_NS(100), TIME_NS_MAX)),
    CMDLINE_INNER_VAR(on_stall, nightmare_on_stall, .desc = "Stall response",
                      .mappings = CMDLINE_MAPPINGS(
                          CMDLINE_MAP("report", NIGHTMARE_ON_STALL_REPORT),
                          CMDLINE_MAP("crash", NIGHTMARE_ON_STALL_CRASH),
                          CMDLINE_MAP("snapshot", NIGHTMARE_ON_STALL_CRASH),
                          CMDLINE_MAP("terminal", NIGHTMARE_ON_STALL_CRASH))),
    CMDLINE_INNER_VAR(boot_index, nightmare_boot_index,
                      .desc = "Opaque campaign boot index"),
    CMDLINE_INNER_STRING(campaign_id, nightmare_campaign_id,
                         .desc = "Opaque campaign identifier"),
    CMDLINE_INNER(perturb,
                  .types = CMDLINE_TYPES(CMDLINE_TYPE_STRING,
                                         CMDLINE_TYPE_LIST),
                  .desc = "Comma-separated perturbation services"));

void nightmare_cmdline_get(struct nightmare_cmdline_config *config) {
    *config = (struct nightmare_cmdline_config){
        .selector = nightmare_selector,
        .intensity = nightmare_intensity,
        .seed = nightmare_seed,
        .seed_present =
            CMDLINE_CHILD(nightmare_root, seed)->status == CMDLINE_ENTRY_FOUND,
        .seed_mode = nightmare_seed_mode,
        .duration_ms = NS_TO_MS(nightmare_duration_ns),
        .drain_grace_ms = NS_TO_MS(nightmare_drain_grace_ns),
        .stat_interval_ms = NS_TO_MS(nightmare_stat_interval_ns),
        .stall_threshold_ms = NS_TO_MS(nightmare_stall_threshold_ns),
        .on_stall = nightmare_on_stall,
        .boot_index = nightmare_boot_index,
        .campaign_id = nightmare_campaign_id,
    };

    struct cmdline_entry *perturb = CMDLINE_CHILD(nightmare_root, perturb);
    if (perturb->status != CMDLINE_ENTRY_FOUND)
        return;

    if (perturb->value.type == CMDLINE_TYPE_LIST) {
        config->perturb_present =
            CMDLINE_EXTRACT(&perturb->value, config->perturb) == ERR_OK;
        return;
    }

    if (perturb->value.type == CMDLINE_TYPE_STRING) {
        config->perturb = (struct cmdline_list){
            .count = 1,
            .items = &perturb->value,
        };
        config->perturb_present = true;
    }
}
