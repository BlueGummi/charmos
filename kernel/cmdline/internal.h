#pragma once

#include <asm.h>
#include <cmdline.h>
#include <console/panic.h>
#include <console/printf.h>
#include <errno.h>
#include <global.h>
#include <kassert.h>
#include <log.h>
#include <math/bit_ops.h>
#include <math/fixed.h>
#include <math/range.h>
#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <parse.h>
#include <string.h>

LINKER_SECTION_DEFINE(struct cmdline_entry, cmdline_entries);
LINKER_SECTION_DEFINE(struct cmdline_schema, cmdline_schemas);

#define MAX_VAR_LEN 128
#define MAX_VAL_LEN 256
#define CMDLINE_MAX_TYPE_ARGS 5

#define CMDLINE_TYPE_LIST_DEF(X)                                               \
    /* Enum,                  Ident,       Arg Hint,       Raw Hint */         \
    X(CMDLINE_TYPE_BOOL, "bool", "<on/off>", "on/off")                         \
    X(CMDLINE_TYPE_INT, "int", "<integer>", "integer")                         \
    X(CMDLINE_TYPE_UINT, "uint", "<unsigned-int>", "unsigned-int")             \
    X(CMDLINE_TYPE_FX, "fx", "<float>", "float")                               \
    X(CMDLINE_TYPE_DURATION, "duration", "<time>", "time")                     \
    X(CMDLINE_TYPE_DATA_SIZE, "data_size", "<size>", "size")                   \
    X(CMDLINE_TYPE_RANGE, "range", "<start-end>", "start-end")                 \
    X(CMDLINE_TYPE_CPU_MASK, "cpu_mask", "<cpus>", "cpus")                     \
    X(CMDLINE_TYPE_MAC, "mac", "<mac-addr>", "mac-addr")                       \
    X(CMDLINE_TYPE_STRING, "string", "<string>", "string")                     \
    X(CMDLINE_TYPE_LIST, "list", "<list>", "list")

static inline const char *cmdline_type_to_str(enum cmdline_type type) {
    switch (type) {
#define _CMDLINE_TYPE_STR(e, ident, ...)                                       \
    case e: return ident;
        CMDLINE_TYPE_LIST_DEF(_CMDLINE_TYPE_STR)
#undef _CMDLINE_TYPE_STR
    case CMDLINE_TYPE_ERR: return "err";
    case CMDLINE_TYPE_NONE: return "none";
    default: return "unknown";
    }
}

static inline const char *cmdline_expr_type_to_str(enum cmdline_type type) {
    switch (type) {
#define _CMDLINE_TYPE_ARG(e, ident, hint, ...)                                 \
    case e: return hint;
        CMDLINE_TYPE_LIST_DEF(_CMDLINE_TYPE_ARG)
#undef _CMDLINE_TYPE_ARG
    default: return "<undefined>";
    }
}
#define CMDLINE_EXPR_TYPE_TO_STR(type) cmdline_expr_type_to_str(type)

static inline const char *cmdline_type_raw_hint(enum cmdline_type type) {
    switch (type) {
#define _CMDLINE_TYPE_RAW(e, ident, hint, raw, ...)                            \
    case e: return raw;
        CMDLINE_TYPE_LIST_DEF(_CMDLINE_TYPE_RAW)
#undef _CMDLINE_TYPE_RAW
    case CMDLINE_TYPE_ERR: return "err";
    case CMDLINE_TYPE_NONE: return "none";
    default: return "unknown";
    }
}

typedef enum errno (*cmdline_parse_fn)(void *write_to, const char *text);
typedef bool (*cmdline_detect_fn)(const char *text, void *out_val);

struct cmdline_type_parser {
    enum cmdline_type type;
    const char *name;
    const char *arg_hint;
    size_t value_size;
    bool uses_allocated_ptr; /* true if heap allocation needed for generic
                                cmdline_value */

    cmdline_detect_fn detect;
    cmdline_parse_fn parse;
};

extern const struct cmdline_type_parser cmdline_parsers[];
extern const size_t cmdline_parsers_count;

static inline bool cmdline_value_is_typed(const struct cmdline_value *val) {
    return val->mode == CMDLINE_MODE_VAR;
}

static inline bool cmdline_entry_is_typed(const struct cmdline_entry *e) {
    return cmdline_value_is_typed(&e->value);
}

static inline enum cmdline_type
cmdline_type_enum_to_cmdline_type(enum type_enum t) {
    switch (t) {
    case TYPE_BOOL: return CMDLINE_TYPE_BOOL;
    case TYPE_INT8:
    case TYPE_INT16:
    case TYPE_INT32:
    case TYPE_INT64: return CMDLINE_TYPE_INT;
    case TYPE_UINT8:
    case TYPE_UINT16:
    case TYPE_UINT32:
    case TYPE_UINT64: return CMDLINE_TYPE_UINT;
    default: return CMDLINE_TYPE_NONE;
    }
}

static inline uint64_t
cmdline_entry_get_accepted_mask(const struct cmdline_entry *e) {
    if (e->types >= (1ULL << CMDLINE_TYPE_OFFSET)) {
        kassert((e->types & ((1ULL << CMDLINE_TYPE_OFFSET) - 1)) == 0);
        return e->types;
    }
    if (e->types >= CMDLINE_TYPE_OFFSET && e->types < CMDLINE_TYPE_NONE) {
        return (1ULL << e->types);
    }
    if (cmdline_value_is_typed(&e->value) && e->value.c_type != TYPE_NONE) {
        switch (e->value.c_type) {
        case TYPE_BOOL: return (1ULL << CMDLINE_TYPE_BOOL);
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT32:
        case TYPE_INT64:
            return (1ULL << CMDLINE_TYPE_INT) | (1ULL << CMDLINE_TYPE_UINT);
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64: return (1ULL << CMDLINE_TYPE_UINT);
        case TYPE_POINTER: return (1ULL << CMDLINE_TYPE_STRING);
        default: break;
        }
    }
    return UINT64_MAX;
}

static inline enum cmdline_type
cmdline_entry_effective_type(const struct cmdline_entry *e) {
    if (e->types != 0 && !(e->types & (e->types - 1)))
        return (enum cmdline_type) __builtin_ctzll(e->types);
    if (e->value.mode == CMDLINE_MODE_VAR)
        return cmdline_type_enum_to_cmdline_type(e->value.c_type);
    return e->value.type;
}

static inline bool cmdline_entry_has_range(const struct cmdline_entry *e) {
    return RANGE_VALID(e->range);
}

static inline size_t
cmdline_extract_type_bits(uint64_t mask,
                          enum cmdline_type out[CMDLINE_MAX_TYPE_ARGS]) {
    size_t found = 0;
    for (size_t bit = CMDLINE_TYPE_OFFSET;
         bit < 64 && found < CMDLINE_MAX_TYPE_ARGS; bit++) {
        if ((mask >> bit) & 1ULL) {
            out[found++] = (enum cmdline_type) bit;
        }
    }
    return found;
}

static inline void cmdline_write_typed_uint(struct cmdline_value *val,
                                            uint64_t raw_val) {
    if (!val->write_to)
        return;
    switch (val->c_type) {
    case TYPE_UINT8: *(uint8_t *) val->write_to = (uint8_t) raw_val; break;
    case TYPE_UINT16: *(uint16_t *) val->write_to = (uint16_t) raw_val; break;
    case TYPE_UINT32: *(uint32_t *) val->write_to = (uint32_t) raw_val; break;
    case TYPE_UINT64: *(uint64_t *) val->write_to = (uint64_t) raw_val; break;
    case TYPE_INT8: *(int8_t *) val->write_to = (int8_t) raw_val; break;
    case TYPE_INT16: *(int16_t *) val->write_to = (int16_t) raw_val; break;
    case TYPE_INT32: *(int32_t *) val->write_to = (int32_t) raw_val; break;
    case TYPE_INT64: *(int64_t *) val->write_to = (int64_t) raw_val; break;
    case TYPE_BOOL: *(bool *) val->write_to = (raw_val != 0); break;
    default: break;
    }
}

static inline bool cmdline_has_choice(const char *const *choices,
                                      const char *val) {
    if (!choices || !val)
        return true;
    for (const char *const *c = choices; *c; c++) {
        if (strcmp(*c, val) == 0)
            return true;
    }
    return false;
}

static inline bool cmdline_lookup_mapping(const struct cmdline_map *mappings,
                                          const char *val, uint64_t *out) {
    if (!mappings || !val)
        return false;
    for (const struct cmdline_map *m = mappings; m->name; m++) {
        if (strcmp(m->name, val) == 0) {
            if (out)
                *out = m->value;
            return true;
        }
    }
    return false;
}

static inline bool cmdline_parse_flags(const struct cmdline_flag *table,
                                       const char *val, uint64_t *out_mask,
                                       const char **err_token,
                                       size_t *err_len) {
    if (!table || !val)
        return false;
    uint64_t mask = 0;
    const char *p = val;
    while (*p) {
        while (*p == ',' || *p == '|' || *p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != ',' && *p != '|' && *p != ' ' && *p != '\t')
            p++;
        size_t len = (size_t) (p - start);

        bool matched = false;
        for (const struct cmdline_flag *f = table; f->name; f++) {
            if (strlen(f->name) == len && strncmp(f->name, start, len) == 0) {
                mask |= f->value;
                matched = true;
                break;
            }
        }
        if (!matched) {
            if (err_token)
                *err_token = start;
            if (err_len)
                *err_len = len;
            return false;
        }
    }
    if (out_mask)
        *out_mask = mask;
    return true;
}

void cmdline_functional_name(const struct cmdline_entry *ent,
                             char name_out[CMDLINE_ENTRY_NAME_LEN_MAX]);
void cmdline_check_for_duplicates(void);
void cmdline_check_for_unfilled(void);
void cmdline_apply_defaults(void);
void cmdline_print_all(void);

void dispatch_parse_value(struct cmdline_entry *e, const char *name,
                          const char *var, const char *val);
void cmdline_assign_all_args(void);
const char *cmdline_intern_composite_arg(uint64_t mask,
                                         const enum cmdline_type *types,
                                         size_t num_types);
struct cmdline_value cmdline_parse_value_for(const char *value,
                                             uint64_t accepted);
struct cmdline_value cmdline_parse_value(struct cmdline_entry *ent,
                                         const char *value);
bool cmdline_has_list_separator(const char *value);
struct cmdline_value cmdline_parse_list(const char *value, uint64_t accepted);

/* Parser functions */
enum errno cmdline_parse_i64(void *write_to, const char *text);
enum errno cmdline_parse_u64(void *write_to, const char *text);
enum errno cmdline_parse_bool(void *write_to, const char *text);
enum errno cmdline_parse_fx(void *write_to, const char *text);
enum errno cmdline_parse_duration(void *write_to, const char *text);
enum errno cmdline_parse_data_size(void *write_to, const char *text);
enum errno cmdline_parse_cpu_mask(void *write_to, const char *text);
enum errno cmdline_parse_mac(void *write_to, const char *text);
enum errno cmdline_parse_range(void *write_to, const char *text);
enum errno cmdline_parse_string(void *write_to, const char *text);

/* Dispatch & Validation prototypes */
void cmdline_dispatch(const char *var, const char *val);
void cmdline_validate_defaults(void);
