/* @title: Command Line */
#pragma once
#include <compiler.h>
#include <errno.h>
#include <linker/symbols.h>
#include <smp/topology.h>
#include <stdbool.h>
#include <stddef.h>
#include <types/type_enum.h>

struct cmdline_entry;

enum cmdline_entry_status {
    CMDLINE_ENTRY_NOT_FOUND = 0,
    CMDLINE_ENTRY_DEFAULTED,
    CMDLINE_ENTRY_FOUND,
};

enum cmdline_entry_flags {
    CMDLINE_ENTRY_FLAGS_NONE = 0,
    CMDLINE_ENTRY_SYMBOLIC = 1, /* This entry serves parenting purposes,
                                 * however itself cannot be set to a value */
    CMDLINE_ENTRY_REQUIRED = 1 << 1,
    CMDLINE_ENTRY_DOCUMENTED = 1 << 2, /* If this is set,
                                        * we print/document it */
};

/* The way this works:
 *
 * A cmdline_value can *either* emit to *write_to OR *data, depending on
 * if it is in type_enum's range or cmdline_value_type's range.
 *
 * If type is already set by the declaration macro, then no cmdline_parse_value
 * will ever run, and the standard parsing will be applied instead.
 *
 */
enum cmdline_value_type {
    CMDLINE_VAL_NONE = TYPE_MAX + 1,
    CMDLINE_VAL_INT,       /* int64_t / uint64_t */
    CMDLINE_VAL_DATA_SIZE, /* Data size in bytes (e.g. 5G) */
    CMDLINE_VAL_BOOL,      /* Boolean */
    CMDLINE_VAL_STRING,    /* Quoted or unquoted string */
    CMDLINE_VAL_RANGE,     /* struct { uint64_t start, end; } */
    CMDLINE_VAL_CPU_MASK,  /* struct cpu_mask */
    CMDLINE_VAL_LIST,      /* Container of sub-values */
    CMDLINE_VAL_ERR,       /* Explicit parse error node */
    CMDLINE_VAL_MAX
};
#define CMDLINE_VALUE_TYPE_BIT(t) (1ULL << (t))

struct cmdline_range {
    uint64_t start;
    uint64_t end;
};

struct cmdline_list {
    size_t count;
    struct cmdline_value *items;
};

struct cmdline_value {
    struct cmdline_entry *entry;

    int type; /* polymorphic */
    union {
        void *write_to;
        void *data;
    };

    union {
        /* this is used when this is in cmdline_parse_value mode */
        uint64_t accepted; /* cmdline_value_type */

        /* this is used when in type_enum mode */
        enum errno (*parse)(void *write_to, const char *text);
    };
};

typedef void (*cmdline_callback)(const char *value, struct cmdline_entry *ent);

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
struct cmdline_entry {
    const char *name;
    const char *desc; /* human readable description */
    const char *arg;  /* value format hint e.g. "<hex bytes>", "<device>" */
    cmdline_callback callback;
    char **raw;
    const char *default_val;

    enum cmdline_entry_flags flags;
    enum cmdline_entry_status status;

    struct cmdline_entry *parent;

    /* Regarding the variable to write to */
    struct cmdline_value value;

    void *private;

    /* NOTE: we could keep track of children, but that's not at all mandatory */
};

#define CMDLINE_ENTRY_DECLARE(n, ...)                                          \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    __cmdline_##n = {.name = #n,                                               \
                     .status = CMDLINE_ENTRY_NOT_FOUND,                        \
                     .value.type = CMDLINE_VAL_NONE,                           \
                     .value.accepted = UINT64_MAX,                             \
                     .flags = CMDLINE_ENTRY_FLAGS_NONE,                        \
                     __VA_ARGS__}

#define CMDLINE_ENTRY_DECLARE_TYPED(n, var, ...)                               \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    __cmdline_##n = {.name = #n,                                               \
                     .status = CMDLINE_ENTRY_NOT_FOUND,                        \
                     .value.write_to = &var,                                   \
                     .value.type = TYPE_TO_ENUM((var)),                        \
                     .flags = CMDLINE_ENTRY_FLAGS_NONE,                        \
                     __VA_ARGS__}

#define CMDLINE_ENTRY_DECLARE_TYPED_CUSTOM(n, var, pars, ...)                  \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    __cmdline_##n = {.name = #n,                                               \
                     .status = CMDLINE_ENTRY_NOT_FOUND,                        \
                     .value.write_to = &var,                                   \
                     .value.parse = pars,                                      \
                     .value.type = TYPE_NONE,                                  \
                     .flags = CMDLINE_ENTRY_FLAGS_NONE,                        \
                     __VA_ARGS__}

#define CMDLINE_EXTRACT(val, var)                                              \
    _Generic(&(var),                                                           \
        bool *: cmdline_extract_bool((val), (bool *) &(var)),                  \
        uint64_t *: cmdline_extract_u64((val), (uint64_t *) &(var)),           \
        int64_t *: cmdline_extract_i64((val), (int64_t *) &(var)),             \
        uint32_t *: cmdline_extract_u32((val), (uint32_t *) &(var)),           \
        int32_t *: cmdline_extract_i32((val), (int32_t *) &(var)),             \
        struct cmdline_range *: cmdline_extract_range(                         \
            (val), (struct cmdline_range *) &(var)),                           \
        char **: cmdline_extract_string((val), (char **) &(var)),              \
        struct cpu_mask *: cmdline_extract_cpu_mask(                           \
            (val), (struct cpu_mask *) &(var)),                                \
        struct cmdline_list *: cmdline_extract_list(                           \
            (val), (struct cmdline_list *) &(var)),                            \
        const char **: cmdline_extract_const_string((val),                     \
                                                    (const char **) &(var)))

#define CMDLINE_ENTRY_DEFINE(n) extern struct cmdline_entry __cmdline_##n

#define CMDLINE_ENTRY(n, ...) &__cmdline_##n

#define CMDLINE_ENTRY_NAME_LEN_MAX 256

#define CMDLINE_ENTRY_TYPE_TO_ARG(type)                                        \
    _Generic((type) 0, int: "<integer>", bool: "<on/off>")

#define cmdline_list_for_each(val, list)                                       \
    for (size_t __i = 0; ((val = (list)->items[__i]), __i < (list)->count);    \
         __i++)

LINKER_SECTION_DEFINE(struct cmdline_entry, cmdline_entries);

extern enum errno (*cmdline_parse_table[TYPE_MAX])(void *write_to,
                                                   const char *text);

enum errno cmdline_parse_i8(void *write_to, const char *text);
enum errno cmdline_parse_u8(void *write_to, const char *text);
enum errno cmdline_parse_i16(void *write_to, const char *text);
enum errno cmdline_parse_u16(void *write_to, const char *text);
enum errno cmdline_parse_i32(void *write_to, const char *text);
enum errno cmdline_parse_u32(void *write_to, const char *text);
enum errno cmdline_parse_i64(void *write_to, const char *text);
enum errno cmdline_parse_u64(void *write_to, const char *text);
enum errno cmdline_parse_bool(void *write_to, const char *text);

enum errno cmdline_parse_float(void *write_to, const char *text);
enum errno cmdline_parse_unsupported(void *write_to, const char *text);

enum errno cmdline_parse_fx(void *write_to, const char *text);

enum errno cmdline_extract_bool(struct cmdline_value *val, bool *out);
enum errno cmdline_extract_u64(struct cmdline_value *val, uint64_t *out);
enum errno cmdline_extract_i64(struct cmdline_value *val, int64_t *out);
enum errno cmdline_extract_u32(struct cmdline_value *val, uint32_t *out);
enum errno cmdline_extract_i32(struct cmdline_value *val, int32_t *out);
enum errno cmdline_extract_range(struct cmdline_value *val,
                                 struct cmdline_range *out);
enum errno cmdline_extract_cpu_mask(struct cmdline_value *val,
                                    struct cpu_mask *out);
enum errno cmdline_extract_string(struct cmdline_value *val, char **out);
enum errno cmdline_extract_const_string(struct cmdline_value *val,
                                        const char **out);
enum errno cmdline_extract_list(struct cmdline_value *val,
                                struct cmdline_list *out);

/* Returns true if the str matches a 'yes' string, and false if it is a 'no',
 * although it's a bit more nuanced than that and matches a lot of things.
 *
 * panics if neither match */
void cmdline_parse(const char *input);
bool cmdline_wants_help(const char *input);
__noreturn void cmdline_dump_help(void);

struct cmdline_value cmdline_parse_value(struct cmdline_entry *ent,
                                         const char *value);

static inline const char *
cmdline_value_type_to_str(enum cmdline_value_type type) {
    switch (type) {
    case CMDLINE_VAL_NONE: return "none";
    case CMDLINE_VAL_INT: return "int";
    case CMDLINE_VAL_DATA_SIZE: return "data_size";
    case CMDLINE_VAL_BOOL: return "bool";
    case CMDLINE_VAL_STRING: return "string";
    case CMDLINE_VAL_RANGE: return "range";
    case CMDLINE_VAL_CPU_MASK: return "cpu_mask";
    case CMDLINE_VAL_LIST: return "list";
    case CMDLINE_VAL_ERR: return "err";
    default: return "<unknown>";
    }
}
