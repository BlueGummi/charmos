#ifdef DEBUG_LOCK_CHK

#include <console/crash.h>
#include <console/printf.h>
#include <ndjson.h>
#include <nightmare/nightmare.h>
#include <stdatomic.h>
#include <string.h>

#include "lock_chk_internal.h"

NDJSON_DECLARE(lock_chk_finding, NDJSON_SECTION_LOCK_CHK, NDJSON_KIND_FINDING,
               1, NDJSON_STR(kind), NDJSON_STR(sig), NDJSON_STR(file),
               NDJSON_U64(line), NDJSON_STR(class), NDJSON_STR(msg),
               NDJSON_STR(mode), NDJSON_U64(cycle_len),
               NDJSON_STR(capacity_pool), NDJSON_U64(capacity_used),
               NDJSON_U64(capacity_limit));

static const char *lock_chk_fail_kind_name(enum lock_chk_failure_kind kind) {
    switch (kind) {
    case LOCK_CHK_FAIL_CYCLE: return "cycle";
    case LOCK_CHK_FAIL_RECURSION: return "recursion";
    case LOCK_CHK_FAIL_RELEASE: return "release";
    case LOCK_CHK_FAIL_CONTEXT: return "context";
    case LOCK_CHK_FAIL_THREAD_EXIT: return "thread_exit";
    case LOCK_CHK_FAIL_SPIN_ORDER: return "spin_order";
    case LOCK_CHK_FAIL_CAPACITY: return "capacity";
    case LOCK_CHK_FAIL_UNINITIALIZED: return "uninitialized";
    }
    return "unknown";
}

static const char *lock_chk_mode_name(enum lock_chk_mode mode) {
    if (mode == LOCK_CHK_MODE_EXCLUSIVE)
        return "exclusive";
    if (mode == LOCK_CHK_MODE_SHARED)
        return "shared";
    return "";
}

static uint64_t
lock_chk_calc_generic_sig(const struct lock_chk_failure *failure) {
    uint64_t signature = HASH_FNV1A_64_OFFSET_BASIS;
    signature =
        lock_chk_hash_string(signature, lock_chk_fail_kind_name(failure->kind));
    if (failure->class != NULL) {
        signature = lock_chk_hash_string(signature, failure->class->name);
        signature = lock_chk_hash_string(signature, failure->class->file);
        signature = lock_chk_hash_bytes(signature, &failure->class->line,
                                        sizeof(failure->class->line));
    }
    if (failure->site != NULL) {
        signature = lock_chk_hash_string(signature, failure->site->file);
        signature = lock_chk_hash_bytes(signature, &failure->site->line,
                                        sizeof(failure->site->line));
    }
    signature = lock_chk_hash_bytes(signature, &failure->subclass,
                                    sizeof(failure->subclass));
    return lock_chk_hash_bytes(signature, &failure->mode,
                               sizeof(failure->mode));
}

static uint64_t
lock_chk_failure_signature(const struct lock_chk_failure *failure) {
    if (failure->signature != 0)
        return failure->signature;
    if (failure->kind == LOCK_CHK_FAIL_CYCLE && failure->cycle_len != 0)
        return lock_chk_calc_canonical_cycle_sig(failure->cycle_hops,
                                                 failure->cycle_len);
    return lock_chk_calc_generic_sig(failure);
}

static void lock_chk_emit_finding(const struct lock_chk_failure *failure,
                                  const char *kind, const char *signature) {
    ndjson_emit(lock_chk_finding, .kind = kind, .sig = signature,
                .file = failure->site ? failure->site->file : "",
                .line = failure->site ? failure->site->line : 0,
                .class = failure->class ? failure->class->name : "",
                .msg = failure->msg, .mode = lock_chk_mode_name(failure->mode),
                .cycle_len = failure->cycle_len,
                .capacity_pool =
                    failure->capacity_pool ? failure->capacity_pool : "",
                .capacity_used = failure->capacity_used,
                .capacity_limit = failure->capacity_limit);
}

static void lock_chk_report_degradation(const struct lock_chk_failure *failure,
                                        uint64_t signature,
                                        const char *signature_text) {
    static _Atomic bool emitted = false;
    if (atomic_exchange_explicit(&emitted, true, memory_order_relaxed))
        return;

    printf_unlocked("\n*** LOCK_CHK WARNING: Capacity exhausted for pool '%s' "
                    "(%u/%u) - entering DEGRADED mode ***\n\n",
                    failure->capacity_pool ? failure->capacity_pool
                                           : "<unknown>",
                    failure->capacity_used, failure->capacity_limit);
    lock_chk_emit_finding(failure, "capacity_degraded", signature_text);
    nightmare_request_external_fail("lock_chk_degraded", signature, "%s",
                                    failure->msg);
}

static void lock_chk_print_cycle(const struct lock_chk_failure *failure) {
    if (failure->kind != LOCK_CHK_FAIL_CYCLE || failure->cycle_len == 0)
        return;

    printf_unlocked("Cycle path (%u hops%s):\n", failure->cycle_len,
                    failure->cycle_truncated ? ", truncated" : "");
    for (uint16_t i = 0; i < failure->cycle_len; i++) {
        const struct lock_chk_cycle_hop *hop = &failure->cycle_hops[i];
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

static void lock_chk_print_failure(const struct lock_chk_failure *failure,
                                   const char *signature) {
    printf_unlocked("\n========================================================"
                    "========================\n");
    printf_unlocked("LOCK_CHK VIOLATION: %s\n", failure->msg);
    printf_unlocked("Signature: 0x%s\n", signature);
    if (failure->site != NULL)
        printf_unlocked("Location: %s:%u\n", failure->site->file,
                        failure->site->line);
    if (failure->class != NULL)
        printf_unlocked("Lock Class: '%s' (%s:%u subclass %u)\n",
                        failure->class->name, failure->class->file,
                        failure->class->line, failure->subclass);

    lock_chk_print_cycle(failure);
    if (failure->kind == LOCK_CHK_FAIL_CAPACITY)
        printf_unlocked("Capacity pool '%s': used %u / limit %u\n",
                        failure->capacity_pool ? failure->capacity_pool
                                               : "<unknown>",
                        failure->capacity_used, failure->capacity_limit);
    printf_unlocked("=========================================================="
                    "======================\n\n");
}

void lock_chk_report_failure(const struct lock_chk_failure *failure) {
    uint64_t signature = lock_chk_failure_signature(failure);
    char signature_text[24];
    snprintf(signature_text, sizeof(signature_text), "%016lx", signature);

    if (failure->kind == LOCK_CHK_FAIL_CAPACITY &&
        !lock_chk_capacity_should_panic()) {
        lock_chk_report_degradation(failure, signature, signature_text);
        return;
    }

    lock_chk_print_failure(failure, signature_text);
    lock_chk_emit_finding(failure, lock_chk_fail_kind_name(failure->kind),
                          signature_text);
    nightmare_request_external_fail("lock_chk", signature, "%s", failure->msg);

    crash(&(struct crash_context){
        .source = CRASH_SOURCE_LOCK_CHK,
        .formats = CRASH_FMT_DEFAULT,
        .file = failure->site ? failure->site->file : __RELFILE__,
        .line = failure->site ? failure->site->line : __LINE__,
        .func = "lock_chk",
        .msg = failure->msg,
        .regs = NULL,
    });
}

#endif /* DEBUG_LOCK_CHK */
