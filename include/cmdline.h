/* @title: Command Line */
#pragma once
#include <compiler.h>
#include <errno.h>
#include <linker/symbols.h>
#include <math/bit.h>
#include <math/fixed.h>
#include <math/range.h>
#include <stdbool.h>
#include <stddef.h>
#include <types/type_enum.h>

struct cmdline_entry;
struct cpu_mask;

enum cmdline_entry_status {
    CMDLINE_ENTRY_NOT_FOUND = 0,
    CMDLINE_ENTRY_DEFAULTED,
    CMDLINE_ENTRY_FOUND,
};

enum cmdline_entry_flags {
    CMDLINE_ENTRY_FLAGS_NONE = 0,
    CMDLINE_ENTRY_SYMBOLIC = 1, /* for namespace parenting purposes */
    CMDLINE_ENTRY_REQUIRED = 1 << 1,
    CMDLINE_ENTRY_HIDDEN = 1 << 2, /* exclude from help message */
};

enum cmdline_value_mode {
    CMDLINE_MODE_POLYMORPHIC = 0,
    CMDLINE_MODE_VAR,
    CMDLINE_MODE_TYPED = CMDLINE_MODE_VAR,
    CMDLINE_MODE_CUSTOM = CMDLINE_MODE_VAR,
};

#define CMDLINE_TYPE_OFFSET 6

enum cmdline_type {
    CMDLINE_TYPE_FIRST_ = CMDLINE_TYPE_OFFSET - 1,
    CMDLINE_TYPE_BOOL = CMDLINE_TYPE_OFFSET,
    CMDLINE_TYPE_INT,
    CMDLINE_TYPE_UINT,
    CMDLINE_TYPE_FX,
    CMDLINE_TYPE_DURATION,
    CMDLINE_TYPE_DATA_SIZE,
    CMDLINE_TYPE_RANGE,
    CMDLINE_TYPE_CPU_MASK,
    CMDLINE_TYPE_MAC,
    CMDLINE_TYPE_STRING,
    CMDLINE_TYPE_LIST,
    CMDLINE_TYPE_ERR,
    CMDLINE_TYPE_NONE,
};

#define CMDLINE_TYPES(...)                                                     \
    _DISPATCH(CMDLINE_IMPL_TYPE_BIT, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

struct cmdline_range {
    uint64_t start;
    uint64_t end;
};

struct cmdline_list {
    size_t count;
    struct cmdline_value *items;
};

struct cmdline_value {
    enum cmdline_value_mode mode;

    union {
        enum type_enum c_type;
        enum cmdline_type type;
    };

    union {
        void *write_to;
        void *data;
        uint64_t u64;
        int64_t i64;
        bool b;
        fx32_32_t fx;
        time_ns_t duration;
    };

    /* this is used when mode == CMDLINE_MODE_TYPED or CMDLINE_MODE_CUSTOM */
    enum errno (*parse)(void *write_to, const char *text);
};

/* The idea with parent-child relationships:
 *
 * The dot is used as a namespace marker. With a given cmdline_entry, we trace
 * its lineage to build out what name it *really* has.
 *
 * For instance, if we have
 *
 * apple->parent = orange
 * orange->parent = pineapple
 * pineapple->parent = NULL
 *
 * the "functional name" of apple is
 *
 * pineapple.orange.apple */
struct __attribute__((aligned(8))) cmdline_entry {
    const char *name;
    const char *desc; /* human readable description */
    const char *arg;  /* value format hint e.g. "<hex bytes>", "<device>" */
    const char *default_val;

    enum cmdline_entry_flags flags;
    enum cmdline_entry_status status;

    struct cmdline_entry *parent;

    uint64_t types;

    struct range range;

    /* Regarding the variable to write to */
    struct cmdline_value value;

    const char *const *choices;
    const struct cmdline_map *mappings;
    const struct cmdline_flag *flags_table;
};

struct cmdline_map {
    const char *name;
    uint64_t value;
};

struct cmdline_flag {
    const char *name;
    uint64_t value;
};

#define CMDLINE_MAP(str, enum_val)                                             \
    {.name = (str), .value = (uint64_t) (enum_val)}
#define CMDLINE_MAPPINGS(...)                                                  \
    ((const struct cmdline_map[]) {__VA_ARGS__, {NULL, 0}})

#define CMDLINE_FLAG(str, bit_val)                                             \
    {.name = (str), .value = (uint64_t) (bit_val)}
#define CMDLINE_FLAGS(...)                                                     \
    ((const struct cmdline_flag[]) {__VA_ARGS__, {NULL, 0}})

#define CMDLINE_CHOICES(...) ((const char *const[]) {__VA_ARGS__, NULL})
#define CMDLINE_PARSER(fn) .value.parse = (fn)

#define CMDLINE_DECLARE(n, ...)                                                \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    __cmdline_##n = {.name = #n,                                               \
                     .status = CMDLINE_ENTRY_NOT_FOUND,                        \
                     .types = 0,                                               \
                     .range = RANGE(1, 0),                                     \
                     .choices = NULL,                                          \
                     .mappings = NULL,                                         \
                     .flags_table = NULL,                                      \
                     .value.mode = CMDLINE_MODE_POLYMORPHIC,                   \
                     .value.type = CMDLINE_TYPE_NONE,                          \
                     .flags = CMDLINE_ENTRY_FLAGS_NONE,                        \
                     __VA_ARGS__}

#define CMDLINE_DECLARE_VAR(n, var, ...)                                       \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    __cmdline_##n = {.name = #n,                                               \
                     .status = CMDLINE_ENTRY_NOT_FOUND,                        \
                     .types = 0,                                               \
                     .range = RANGE(1, 0),                                     \
                     .choices = NULL,                                          \
                     .mappings = NULL,                                         \
                     .flags_table = NULL,                                      \
                     .value.mode = CMDLINE_MODE_VAR,                           \
                     .value.write_to = &(var),                                 \
                     .value.c_type = TYPE_TO_ENUM((var)),                      \
                     .value.parse = NULL,                                      \
                     .flags = CMDLINE_ENTRY_FLAGS_NONE,                        \
                     __VA_ARGS__}

#define CMDLINE_CHILD_DECLARE(parent_n, n, ...)                                \
    CMDLINE_DECLARE(parent_n##_##n, .name = #n, .parent = CMDLINE(parent_n),   \
                    __VA_ARGS__)

#define CMDLINE_CHILD_DECLARE_VAR(parent_n, n, var, ...)                       \
    CMDLINE_DECLARE_VAR(parent_n##_##n, var, .name = #n,                       \
                        .parent = CMDLINE(parent_n), __VA_ARGS__)

#define CMDLINE_INNER(n, ...) (_CMDLINE_CHILD_KIND_POLY, n, 0, ##__VA_ARGS__)

#define CMDLINE_INNER_VAR(n, var, ...)                                         \
    (_CMDLINE_CHILD_KIND_VAR, n, var, ##__VA_ARGS__)

#define CMDLINE_CHILDREN_DECLARE(parent_n, ...)                                \
    _DISPATCH(_CMDLINE_CHILDREN_MAP, PP_NARG(__VA_ARGS__))(parent_n,           \
                                                           __VA_ARGS__)

#define CMDLINE_EXTRACT(val, var)                                              \
    _Generic(&(var),                                                           \
        bool *: cmdline_extract_bool((val), (bool *) &(var)),                  \
        uint64_t *: cmdline_extract_u64((val), (uint64_t *) &(var)),           \
        int64_t *: cmdline_extract_i64((val), (int64_t *) &(var)),             \
        uint32_t *: cmdline_extract_u32((val), (uint32_t *) &(var)),           \
        int32_t *: cmdline_extract_i32((val), (int32_t *) &(var)),             \
        uint16_t *: cmdline_extract_u16((val), (uint16_t *) &(var)),           \
        int16_t *: cmdline_extract_i16((val), (int16_t *) &(var)),             \
        uint8_t *: cmdline_extract_u8((val), (uint8_t *) &(var)),              \
        int8_t *: cmdline_extract_i8((val), (int8_t *) &(var)),                \
        struct cmdline_range *: cmdline_extract_range(                         \
            (val), (struct cmdline_range *) &(var)),                           \
        char **: cmdline_extract_string((val), (char **) &(var)),              \
        struct cpu_mask *: cmdline_extract_cpu_mask(                           \
            (val), (struct cpu_mask *) &(var)),                                \
        struct cmdline_list *: cmdline_extract_list(                           \
            (val), (struct cmdline_list *) &(var)),                            \
        const char **: cmdline_extract_const_string((val),                     \
                                                    (const char **) &(var)))

#define CMDLINE_EXTRACT_LIST(list, type, out_buf, max_count, parse_fn)         \
    ({                                                                         \
        size_t __n = 0;                                                        \
        const struct cmdline_list *__l = (list);                               \
        const size_t __max = (max_count);                                      \
        for (size_t __i = 0; __i < __l->count && __n < __max; __i++) {         \
            type __v = (type) {0};                                             \
            if ((parse_fn) (&__l->items[__i], &__v) == ERR_OK)                 \
                (out_buf)[__n++] = __v;                                        \
        }                                                                      \
        __n;                                                                   \
    })

#define CMDLINE_NODE_1(a) a
#define CMDLINE_NODE_2(a, b) a##_##b
#define CMDLINE_NODE_3(a, b, c) a##_##b##_##c
#define CMDLINE_NODE_4(a, b, c, d) a##_##b##_##c##_##d
#define CMDLINE_NODE(...)                                                      \
    _DISPATCH(CMDLINE_NODE, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define CMDLINE_DEFINE(n) extern struct cmdline_entry CONCAT(__cmdline_, n)
#define CMDLINE(n) (&CONCAT(__cmdline_, n))
#define CMDLINE_VALUE(n) cmdline_entry_value_u64(CMDLINE(n))

#define CMDLINE_CHILD_DEFINE(...) CMDLINE_DEFINE(CMDLINE_NODE(__VA_ARGS__))
#define CMDLINE_CHILD(...) CMDLINE(CMDLINE_NODE(__VA_ARGS__))
#define CMDLINE_CHILD_VALUE(...)                                               \
    cmdline_entry_value_u64(CMDLINE_CHILD(__VA_ARGS__))

#define CMDLINE_ENTRY_NAME_LEN_MAX 256

#define cmdline_list_for_each(val, list)                                       \
    for (size_t __i = 0, __count = (list)->count;                              \
         __i < __count && (((val) = (list)->items[__i]), true); __i++)

/* Schema-Driven Subsystem Definitions */
struct cmdline_schema_prop {
    const char *name;
    const char *desc;
    size_t offset;
    enum type_enum c_type;
    uint64_t types;
    enum errno (*parse)(void *dst, const char *text);

    struct range range;

    const char *const *choices;
    const struct cmdline_map *mappings;
    const struct cmdline_flag *flags_table;
};

typedef void *(*cmdline_instance_resolver_t)(const char *path, size_t path_len);

struct __attribute__((aligned(8))) cmdline_schema {
    const char *prefix;
    const char *path_hint;
    const char *desc;
    cmdline_instance_resolver_t resolve;
    const struct cmdline_schema_prop *props;
    size_t prop_count;
};

#define CMDLINE_SCHEMA_PROP_PARSER(fn) .parse = (fn)

#define CMDLINE_SCHEMA_PROP(struct_type, member, ...)                          \
    {.name = #member,                                                          \
     .offset = offsetof(struct_type, member),                                  \
     .c_type = TYPE_TO_ENUM(((struct_type *) 0)->member),                      \
     .types = 0,                                                               \
     .parse = NULL,                                                            \
     .range = RANGE(1, 0),                                                     \
     .choices = NULL,                                                          \
     .mappings = NULL,                                                         \
     .flags_table = NULL,                                                      \
     __VA_ARGS__}

#define CMDLINE_SCHEMA_DECLARE(n, prefix_str, path_hint_str, desc_str,         \
                               resolver_fn, ...)                               \
    static const struct cmdline_schema_prop __cmdline_schema_props_##n[] = {   \
        __VA_ARGS__};                                                          \
    LINKER_SECTION_OBJECT(struct cmdline_schema, cmdline_schemas)              \
    __cmdline_schema_##n = {                                                   \
        .prefix = (prefix_str),                                                \
        .path_hint = (path_hint_str),                                          \
        .desc = (desc_str),                                                    \
        .resolve = (resolver_fn),                                              \
        .props = __cmdline_schema_props_##n,                                   \
        .prop_count = sizeof(__cmdline_schema_props_##n) /                     \
                      sizeof(__cmdline_schema_props_##n[0]),                   \
    }

#define CMDLINE_GET(key, type, fallback)                                       \
    ({                                                                         \
        type __out = (fallback);                                               \
        struct cmdline_entry *__e = cmdline_lookup(key);                       \
        if (__e && (__e->status == CMDLINE_ENTRY_FOUND ||                      \
                    __e->status == CMDLINE_ENTRY_DEFAULTED)) {                 \
            CMDLINE_EXTRACT(&__e->value, __out);                               \
        }                                                                      \
        __out;                                                                 \
    })

#define cmdline_read_or(key, target_var, fallback)                             \
    ({                                                                         \
        bool __overridden = false;                                             \
        struct cmdline_entry *__e = cmdline_lookup(key);                       \
        if (__e && __e->status == CMDLINE_ENTRY_FOUND &&                       \
            CMDLINE_EXTRACT(&__e->value, target_var) == ERR_OK) {              \
            __overridden = true;                                               \
        } else {                                                               \
            (target_var) = (fallback);                                         \
        }                                                                      \
        __overridden;                                                          \
    })

void cmdline_parse(const char *input);
bool cmdline_wants_help(const char *input);
__noreturn void cmdline_dump_help(void);
void cmdline_debug_hook(void);

struct cmdline_entry *cmdline_lookup(const char *key);
uint64_t cmdline_entry_value_u64(const struct cmdline_entry *e);

#include "cmdline_api_internal.h"
