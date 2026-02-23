#include <bootstage.h>
#include <console/printf.h>
#include <global.h>
#include <log.h>

static const char *bootstage_str[BOOTSTAGE_COUNT] = {
    [BOOTSTAGE_EARLY_FB] = "Early - Framebuffer",
    [BOOTSTAGE_EARLY_ALLOCATORS] = "Early - Allocators",
    [BOOTSTAGE_EARLY_DEVICES] = "Early - Devices",
    [BOOTSTAGE_MID_SCHEDULER] = "Mid - Scheduler",
    [BOOTSTAGE_MID_MP] = "Mid - SMP",
    [BOOTSTAGE_MID_TOPOLOGY] = "Mid - Topology",
    [BOOTSTAGE_MID_ALLOCATORS] = "Mid - Allocators",
    [BOOTSTAGE_LATE] = "Late",
    [BOOTSTAGE_COMPLETE] = "Complete",
};

static LOG_HANDLE_DECLARE_DEFAULT(bootstage);
void bootstage_advance(enum bootstage new) {
    global.current_bootstage = new;
    log_info_global(LOG_HANDLE(bootstage), "Reached bootstage \'%s\'",
                    bootstage_str[new]);
}
