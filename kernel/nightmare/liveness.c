#include "internal.h"

#include <nightmare/record.h>

#ifdef TEST_NIGHTMARE_ENABLED
void nightmare_report_stall(const struct nightmare_stall_evidence *evidence) {
    NIGHTMARE_FINDING_TIER(
        "stall", NIGHTMARE_TIER_AMBIGUOUS, evidence->worker_index,
        "cpu=%zu tid=%zu worker=%zu role=%s last_progress_ms=%lu progress=%lu",
        evidence->cpu, evidence->tid, evidence->worker_index,
        evidence->role ? evidence->role : "unknown", evidence->last_progress_ms,
        evidence->progress);
    nightmare_publish_stop(NM_STOP_STALL);
}
#endif
