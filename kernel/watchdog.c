#include <cmdline.h>
#include <time/time.h>
#include <watchdog.h>

static struct watchdog_config config = {0};
static struct watchdog_master watchdog_master = {0};

static CMDLINE_DECLARE(watchdog, .flags = CMDLINE_ENTRY_SYMBOLIC,
                       .desc = "Watchdog command line namespace");

CMDLINE_CHILD_DECLARE(watchdog, master, .flags = CMDLINE_ENTRY_SYMBOLIC);

CMDLINE_CHILDREN_DECLARE(
    CMDLINE_NODE(watchdog, master),
    CMDLINE_INNER_DURATION(heartbeat_interval, config.master_heartbeat_interval,
                           .range = RANGE(MS_TO_NS(1), SECONDS_TO_NS(60))),
    CMDLINE_INNER_DURATION(bucket_interval, config.bucket_interval,
                           .range = RANGE(MS_TO_NS(1), SECONDS_TO_NS(60))),
    CMDLINE_INNER_FX(panic_score, config.master_panic_score,
                     .range = RANGE(0, FX_ONE)),
    CMDLINE_INNER_FX(warn_score, config.master_warn_score,
                     .range = RANGE(0, FX_ONE)),
    CMDLINE_INNER_FX(suspect_threshold, config.master_suspect_threshold,
                     .range = RANGE(0, FX_ONE)));

void watchdog_init(void) {}
