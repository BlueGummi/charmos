#include <bootstage.h>
#include <global.h>
#include <console/printf.h>

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

void bootstage_advance(enum bootstage new) {
    global.current_bootstage = new;
    k_info("BOOTSTAGE", K_INFO, "Reached bootstage \'%s\'", bootstage_str[new]);
}
