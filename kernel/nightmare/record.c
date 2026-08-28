#include <ndjson.h>
#include <nightmare/record.h>

NDJSON_DECLARE(nightmare_boot, NDJSON_SECTION_NIGHTMARE, NDJSON_KIND_BOOT, 1,
               NDJSON_STR(test), NDJSON_HEX(seed), NDJSON_STR(seed_policy),
               NDJSON_STR(seed_mode), NDJSON_I64(intensity),
               NDJSON_U64(intensity_val), NDJSON_U64(workers), NDJSON_U64(smp),
               NDJSON_U64(mem_mib), NDJSON_STR(caps), NDJSON_STR(campaign_id),
               NDJSON_U64(boot_index), NDJSON_STR(commit));

NDJSON_DECLARE(nightmare_stat, NDJSON_SECTION_NIGHTMARE, NDJSON_KIND_STAT, 1,
               NDJSON_U64(progress), NDJSON_U64(workers));

NDJSON_DECLARE(nightmare_quiesce, NDJSON_SECTION_NIGHTMARE, NDJSON_KIND_QUIESCE,
               1, NDJSON_STR(result), NDJSON_U64(checks));

NDJSON_DECLARE(nightmare_finding, NDJSON_SECTION_NIGHTMARE, NDJSON_KIND_FINDING,
               1, NDJSON_STR(kind), NDJSON_STR(tier), NDJSON_STR(sig),
               NDJSON_STR(site), NDJSON_STR(msg));

NDJSON_DECLARE(nightmare_verdict, NDJSON_SECTION_NIGHTMARE, NDJSON_KIND_VERDICT,
               1, NDJSON_STR(result), NDJSON_STR(reason),
               NDJSON_U64(duration_ms), NDJSON_U64(progress),
               NDJSON_U64(findings), NDJSON_STR(msg));

void nightmare_record_boot(const struct nightmare_boot_record *r) {
    ndjson_emit(nightmare_boot, .test = r->test, .seed = r->seed,
                .seed_policy = r->seed_policy, .seed_mode = r->seed_mode,
                .intensity = r->intensity, .intensity_val = r->intensity_val,
                .workers = r->workers, .smp = r->smp, .mem_mib = r->mem_mib,
                .caps = r->caps, .campaign_id = r->campaign_id,
                .boot_index = r->boot_index, .commit = r->commit);
}

void nightmare_record_stat(uint64_t progress, uint64_t workers) {
    ndjson_emit(nightmare_stat, .progress = progress, .workers = workers);
}

void nightmare_record_quiesce(const struct nightmare_quiesce_record *r) {
    ndjson_emit(nightmare_quiesce, .result = r->result, .checks = r->checks);
}

void nightmare_record_finding(const struct nightmare_finding_record *r) {
    ndjson_emit(nightmare_finding, .kind = r->kind, .tier = r->tier,
                .sig = r->sig, .site = r->site, .msg = r->msg);
}

void nightmare_record_verdict(const struct nightmare_verdict_record *r) {
    ndjson_emit(nightmare_verdict, .result = r->result, .reason = r->reason,
                .duration_ms = r->duration_ms, .progress = r->progress,
                .findings = r->findings, .msg = r->msg);
}
