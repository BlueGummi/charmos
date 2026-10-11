#ifdef DEBUG_LOCK_CHK

#include <atomic.h>
#include <console/crash.h>
#include <console/printf.h>
#include <kassert.h>
#include <ndjson.h>
#include <nightmare/nightmare.h>

#include "internal.h"

NDJSON_DEFINE(lock_chk_finding, NDJSON_SECTION_LOCK_CHK, NDJSON_KIND_FINDING, 1,
              NDJSON_STR(kind), NDJSON_STR(sig), NDJSON_STR(file),
              NDJSON_U64(line), NDJSON_STR(class), NDJSON_STR(msg),
              NDJSON_STR(mode), NDJSON_U64(cycle_len),
              NDJSON_STR(capacity_pool), NDJSON_U64(capacity_used),
              NDJSON_U64(capacity_limit));

static const char *lock_chk_mode_name(enum lock_chk_mode mode) {
    if (mode == LOCK_CHK_MODE_EXCLUSIVE)
        return "exclusive";
    if (mode == LOCK_CHK_MODE_SHARED)
        return "shared";
    return "";
}

static const char *lock_chk_fault_msg(const struct lock_chk_fault *fault) {
    if (fault->report == NULL || fault->report->msg[0] == '\0')
        return "lock_chk failure (report buffer busy)";

    return fault->report->msg;
}

static uint16_t lock_chk_fault_cycle_len(const struct lock_chk_fault *fault) {
    return fault->report != NULL ? fault->report->cycle_len : 0;
}

static uint64_t lock_chk_calc_generic_sig(const struct lock_chk_fault *fault) {
    uint64_t signature = HASH_FNV1A_64_OFFSET_BASIS;
    signature =
        lock_chk_hash_str(signature, lock_chk_fail_kind_to_str(fault->kind));
    const struct lock_chk_class *class = fault->class;
    if (class != NULL) {
        signature = lock_chk_hash_str(signature, class->name);
        signature = lock_chk_hash_str(signature, class->file);
        signature =
            lock_chk_hash_bytes(signature, &class->line, sizeof(class->line));
    }
    if (fault->site != NULL) {
        signature = lock_chk_hash_str(signature, fault->site->file);
        signature = lock_chk_hash_bytes(signature, &fault->site->line,
                                        sizeof(fault->site->line));
    }
    signature = lock_chk_hash_bytes(signature, &fault->subclass,
                                    sizeof(fault->subclass));
    return lock_chk_hash_bytes(signature, &fault->mode, sizeof(fault->mode));
}

static uint64_t lock_chk_fail_sig(const struct lock_chk_fault *fault) {
    const struct lock_chk_report *report = fault->report;
    if (report != NULL) {
        if (report->signature != 0)
            return report->signature;
        if (fault->kind == LOCK_CHK_FAIL_CYCLE && report->cycle_len != 0)
            return lock_chk_calc_sig(report->cycle_hops, report->cycle_len);
    }

    return lock_chk_calc_generic_sig(fault);
}

static void lock_chk_emit_finding(const struct lock_chk_fault *fault,
                                  const char *kind, const char *signature) {
    ndjson_emit(lock_chk_finding, .kind = kind, .sig = signature,
                .file = fault->site ? fault->site->file : "",
                .line = fault->site ? fault->site->line : 0,
                .class = fault->class ? fault->class->name : "",
                .msg = lock_chk_fault_msg(fault),
                .mode = lock_chk_mode_name(fault->mode),
                .cycle_len = lock_chk_fault_cycle_len(fault),
                .capacity_pool =
                    fault->capacity_pool ? fault->capacity_pool : "",
                .capacity_used = fault->capacity_used,
                .capacity_limit = fault->capacity_limit);
}

static void maybe_report_degradation(const struct lock_chk_fault *fault,
                                     uint64_t signature,
                                     const char *signature_text) {
    static atomic_bool emitted = false;
    if (atomic_xchg_rlx(&emitted, true))
        return;

    printf_unlocked("\n*** LOCK_CHK WARNING: Capacity exhausted for pool '%s' "
                    "(%u/%u) because DEGRADED***\n\n",
                    fault->capacity_pool ? fault->capacity_pool : "<unknown>",
                    fault->capacity_used, fault->capacity_limit);
    lock_chk_emit_finding(fault, "capacity_degraded", signature_text);
    nightmare_request_external_fail("lock_chk_degraded", signature, "%s",
                                    lock_chk_fault_msg(fault));
}

static void print_cycle(const struct lock_chk_fault *fault) {
    const struct lock_chk_report *report = fault->report;
    if (fault->kind != LOCK_CHK_FAIL_CYCLE || report == NULL ||
        report->cycle_len == 0)
        return;

    printf_unlocked("Cycle path (%u hops%s):\n", report->cycle_len,
                    report->cycle_truncated ? ", truncated" : "");
    for (uint16_t i = 0; i < report->cycle_len; i++) {
        const struct lock_chk_cycle_hop *hop = &report->cycle_hops[i];
        printf_unlocked(
            "  [%u] '%s' (%s:%u sc %u) [%s]\n", i,
            hop->from_class ? hop->from_class->name : "<unknown>",
            hop->from_class ? hop->from_class->file : "?",
            hop->from_class ? hop->from_class->line : 0, hop->from_subclass,
            hop->from_mode == LOCK_CHK_MODE_EXCLUSIVE ? "EXCLUSIVE" : "SHARED");
        printf_unlocked(
            "      -> '%s' (%s:%u sc %u) [%s] at %s:%u\n",
            hop->to_class ? hop->to_class->name : "<unknown>",
            hop->to_class ? hop->to_class->file : "?",
            hop->to_class ? hop->to_class->line : 0, hop->to_subclass,
            hop->to_mode == LOCK_CHK_MODE_EXCLUSIVE ? "EXCLUSIVE" : "SHARED",
            hop->site ? hop->site->file : "?", hop->site ? hop->site->line : 0);
    }
}

static void print_fail(const struct lock_chk_fault *fault,
                       const char *signature) {
    printf_unlocked("\n========================================================"
                    "========================\n");
    printf_unlocked("LOCK_CHK VIOLATION: %s\n", lock_chk_fault_msg(fault));
    printf_unlocked("Signature: 0x%s\n", signature);
    if (fault->site != NULL)
        printf_unlocked("Location: %s:%u\n", fault->site->file,
                        fault->site->line);

    const struct lock_chk_class *class = fault->class;
    if (class)
        printf_unlocked("Lock Class: '%s' (%s:%u subclass %u)\n", class->name,
                        class->file, class->line, fault->subclass);

    print_cycle(fault);
    if (fault->kind == LOCK_CHK_FAIL_CAPACITY)
        printf_unlocked("Capacity pool '%s': used %u / limit %u\n",
                        fault->capacity_pool ? fault->capacity_pool
                                             : "<unknown>",
                        fault->capacity_used, fault->capacity_limit);
    printf_unlocked("=========================================================="
                    "======================\n\n");
}

void lock_chk_report_fail(const struct lock_chk_fault *fault) {
    uint64_t signature = lock_chk_fail_sig(fault);
    char signature_text[24];
    snprintf(signature_text, sizeof(signature_text), "%016lx", signature);

    if (fault->kind == LOCK_CHK_FAIL_CAPACITY &&
        !lock_chk_global.panic_on_exhaustion) {
        maybe_report_degradation(fault, signature, signature_text);
        lock_chk_report_release();
        return;
    }

    print_fail(fault, signature_text);
    lock_chk_emit_finding(fault, lock_chk_fail_kind_to_str(fault->kind),
                          signature_text);
    nightmare_request_external_fail("lock_chk", signature, "%s",
                                    lock_chk_fault_msg(fault));

    crash_full(&(struct crash_context){
        .source = CRASH_SOURCE_LOCK_CHK,
        .formats = CRASH_FMT_DEFAULT,
        .file = fault->site ? fault->site->file : __RELFILE__,
        .line = fault->site ? fault->site->line : __LINE__,
        .func = "lock_chk",
        .msg = lock_chk_fault_msg(fault),
        .regs = NULL,
    });
}

#endif /* DEBUG_LOCK_CHK */
