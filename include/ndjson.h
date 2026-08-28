/* @title: NDJSON */
#pragma once
#include <compiler.h>
#include <linker/symbols.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Uses a non-console output and a single
 * JSON object for each line, with each record type declared once (custom)
 * to provide custom member fields.
 *
 * e.g.
 *
 *   NDJSON_DECLARE(test_result, "test", "result", 1,
 *                  NDJSON_STR(name),
 *                  NDJSON_STR(status),
 *                  NDJSON_U64(duration_ms));
 *
 *   ndjson_emit(test_result, .name = t->name, .status = "pass",
 *               .duration_ms = took);
 *
 * Omitted fields are left as zero
 */

#define NDJSON_KEY_DOMAIN "d"
#define NDJSON_KEY_KIND "k"
#define NDJSON_KEY_VERSION "v"
#define NDJSON_KEY_TIME "t"
#define NDJSON_KEY_CPU "c"
#define NDJSON_KEY_TRUNCATED "_trunc"

#define NDJSON_DOMAIN_NDJSON "ndjson"
#define NDJSON_DOMAIN_TEST "test"
#define NDJSON_DOMAIN_PANIC "panic"
#define NDJSON_DOMAIN_ASAN "asan"
#define NDJSON_DOMAIN_SELFTEST "selftest"
#define NDJSON_DOMAIN_NIGHTMARE "nightmare"
#define NDJSON_DOMAIN_LOCK_CHK "lock_chk"
#define NDJSON_DOMAIN_LOG "log"

#define NDJSON_KIND_SCHEMA "schema"
#define NDJSON_KIND_BYE "bye"
#define NDJSON_KIND_BEGIN "begin"
#define NDJSON_KIND_RESULT "result"
#define NDJSON_KIND_GROUP_START "group_start"
#define NDJSON_KIND_GROUP_END "group_end"
#define NDJSON_KIND_TOTALS "totals"
#define NDJSON_KIND_VERDICT "verdict"
#define NDJSON_KIND_EXIT "exit"
#define NDJSON_KIND_AT "at"
#define NDJSON_KIND_FAULT "fault"
#define NDJSON_KIND_FRAME "frame"
#define NDJSON_KIND_PEER "peer"
#define NDJSON_KIND_OWNER "owner"
#define NDJSON_KIND_BOOT "boot"
#define NDJSON_KIND_STAT "stat"
#define NDJSON_KIND_QUIESCE "quiesce"
#define NDJSON_KIND_FINDING "finding"
#define NDJSON_KIND_MESSAGE "msg"

#define NDJSON_TYPE_NAME_U64 "u64"
#define NDJSON_TYPE_NAME_I64 "i64"
#define NDJSON_TYPE_NAME_BOOL "bool"
#define NDJSON_TYPE_NAME_STR "str"
#define NDJSON_TYPE_NAME_HEX "hex"

enum ndjson_type : uint8_t {
    NDJSON_TYPE_U64,
    NDJSON_TYPE_I64,
    NDJSON_TYPE_BOOL,
    NDJSON_TYPE_STR,
    NDJSON_TYPE_HEX, /* emitted as "0x.." */
};

struct ndjson_field {
    const char *name;
    enum ndjson_type type;
    uint16_t offset; /* into the generated argument struct */
};

struct ndjson_record {
    const char *domain;
    const char *kind;
    uint16_t version;
    uint16_t nfields;
    const struct ndjson_field *fields;
};

LINKER_SECTION_DEFINE(struct ndjson_record, ndjson_records);

#define NDJSON_CTYPE_U64 uint64_t
#define NDJSON_CTYPE_I64 int64_t
#define NDJSON_CTYPE_BOOL bool
#define NDJSON_CTYPE_STR const char *
#define NDJSON_CTYPE_HEX uint64_t

/* NDJSON_DECLARE uses this */
#define NDJSON_U64(n) (U64, n)
#define NDJSON_I64(n) (I64, n)
#define NDJSON_BOOL(n) (BOOL, n)
#define NDJSON_STR(n) (STR, n)
#define NDJSON_HEX(n) (HEX, n)

#define NDJSON_UNTUPLE(...) __VA_ARGS__

#define NDJSON_MEMBER(id, spec) NDJSON_MEMBER_(id, NDJSON_UNTUPLE spec)
#define NDJSON_MEMBER_(id, ...) NDJSON_MEMBER__(id, __VA_ARGS__)
#define NDJSON_MEMBER__(id, t, n) NDJSON_CTYPE_##t n;

#define NDJSON_DESC(id, spec) NDJSON_DESC_(id, NDJSON_UNTUPLE spec)
#define NDJSON_DESC_(id, ...) NDJSON_DESC__(id, __VA_ARGS__)
#define NDJSON_DESC__(id, t, n)                                                \
    {.name = #n,                                                               \
     .type = NDJSON_TYPE_##t,                                                  \
     .offset = (uint16_t) __builtin_offsetof(struct __ndjson_args_##id, n)},

/* Apply f(a, x) to each x with constant a */
#define NDJSON_MAP_1(f, a, x) f(a, x)
#define NDJSON_MAP_2(f, a, x, ...) f(a, x) NDJSON_MAP_1(f, a, __VA_ARGS__)
#define NDJSON_MAP_3(f, a, x, ...) f(a, x) NDJSON_MAP_2(f, a, __VA_ARGS__)
#define NDJSON_MAP_4(f, a, x, ...) f(a, x) NDJSON_MAP_3(f, a, __VA_ARGS__)
#define NDJSON_MAP_5(f, a, x, ...) f(a, x) NDJSON_MAP_4(f, a, __VA_ARGS__)
#define NDJSON_MAP_6(f, a, x, ...) f(a, x) NDJSON_MAP_5(f, a, __VA_ARGS__)
#define NDJSON_MAP_7(f, a, x, ...) f(a, x) NDJSON_MAP_6(f, a, __VA_ARGS__)
#define NDJSON_MAP_8(f, a, x, ...) f(a, x) NDJSON_MAP_7(f, a, __VA_ARGS__)
#define NDJSON_MAP_9(f, a, x, ...) f(a, x) NDJSON_MAP_8(f, a, __VA_ARGS__)
#define NDJSON_MAP_10(f, a, x, ...) f(a, x) NDJSON_MAP_9(f, a, __VA_ARGS__)
#define NDJSON_MAP_11(f, a, x, ...) f(a, x) NDJSON_MAP_10(f, a, __VA_ARGS__)
#define NDJSON_MAP_12(f, a, x, ...) f(a, x) NDJSON_MAP_11(f, a, __VA_ARGS__)
#define NDJSON_MAP_13(f, a, x, ...) f(a, x) NDJSON_MAP_12(f, a, __VA_ARGS__)
#define NDJSON_MAP_14(f, a, x, ...) f(a, x) NDJSON_MAP_13(f, a, __VA_ARGS__)
#define NDJSON_MAP_15(f, a, x, ...) f(a, x) NDJSON_MAP_14(f, a, __VA_ARGS__)
#define NDJSON_MAP_16(f, a, x, ...) f(a, x) NDJSON_MAP_15(f, a, __VA_ARGS__)

#define NDJSON_MAP(f, a, ...)                                                  \
    _DISPATCH(NDJSON_MAP, PP_NARG(__VA_ARGS__))(f, a, __VA_ARGS__)

#define NDJSON_MAX_FIELDS 16

#define NDJSON_DECLARE(id, domain_, kind_, version_, ...)                      \
    struct __ndjson_args_##id {                                                \
        NDJSON_MAP(NDJSON_MEMBER, id, __VA_ARGS__)                             \
    };                                                                         \
    static const struct ndjson_field __ndjson_fields_##id[] = {                \
        NDJSON_MAP(NDJSON_DESC, id, __VA_ARGS__)};                             \
    LINKER_SECTION_OBJECT(struct ndjson_record, ndjson_records)                \
    __ndjson_rec_##id = {.domain = (domain_),                                  \
                         .kind = (kind_),                                      \
                         .version = (version_),                                \
                         .nfields = PP_NARG(__VA_ARGS__),                      \
                         .fields = __ndjson_fields_##id};                      \
    static_assert(PP_NARG(__VA_ARGS__) <= NDJSON_MAX_FIELDS,                   \
                  "ndjson record " #id " has too many fields")

#define NDJSON(id) (&__ndjson_rec_##id)
#define NDJSON_EXTERN(id) extern struct ndjson_record __ndjson_rec_##id

void ndjson_emit_impl(const struct ndjson_record *rec, const void *args);

#define ndjson_emit(id, ...)                                                   \
    ndjson_emit_impl(NDJSON(id),                                               \
                     &(const struct __ndjson_args_##id) {__VA_ARGS__})

#define NDJSON_STR_MAX 512

/* Terminator, and signals that the kernel run ended, used so the logs know
 * that something failed/hung when it is absent */
void ndjson_bye(uint64_t code, const char *reason);

void ndjson_early_init(void);
void ndjson_init(void);

bool ndjson_carrier_online(void);
void ndjson_carrier_init(uint16_t port);
void ndjson_carrier_disable(void);

void ndjson_enter_panic(void);
