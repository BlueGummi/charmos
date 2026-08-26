/* @title: Nightmare machine records */
#pragma once
#include <stdint.h>

struct nightmare_boot_record {
    const char *test;
    uint64_t seed;
    const char *seed_policy;
    const char *seed_mode;
    int64_t intensity;
    uint64_t intensity_val;
    uint64_t workers;
    uint64_t smp;
    uint64_t mem_mib;
    const char *caps;
    const char *campaign_id;
    uint64_t boot_index;
    const char *commit;
};

struct nightmare_quiesce_record {
    const char *result;
    uint64_t checks;
};

struct nightmare_finding_record {
    const char *kind;
    const char *tier;
    const char *sig;
    const char *site;
    const char *msg;
};

struct nightmare_verdict_record {
    const char *result;
    const char *reason;
    uint64_t duration_ms;
    uint64_t progress;
    uint64_t findings;
    const char *msg;
};

void nightmare_record_boot(const struct nightmare_boot_record *record);
void nightmare_record_stat(uint64_t progress, uint64_t workers);
void nightmare_record_quiesce(const struct nightmare_quiesce_record *record);
void nightmare_record_finding(const struct nightmare_finding_record *record);
void nightmare_record_verdict(const struct nightmare_verdict_record *record);
