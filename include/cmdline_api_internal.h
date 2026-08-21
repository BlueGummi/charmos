/* @title: Command Line Macro Internals */
#pragma once
#include <compiler.h>
#include <types/type_enum.h>

struct cmdline_entry;
struct cmdline_value;
struct cmdline_list;
struct cmdline_range;
struct cpu_mask;

/* ========== Type Offset + Bits ========== */

#define CMDLINE_TYPE_OFFSET 6

#define CMDLINE_IMPL_TYPE_BIT_1(t1) (1ULL << (t1))
#define CMDLINE_IMPL_TYPE_BIT_2(t1, t2) ((1ULL << (t1)) | (1ULL << (t2)))
#define CMDLINE_IMPL_TYPE_BIT_3(t1, t2, t3)                                    \
    (CMDLINE_IMPL_TYPE_BIT_2(t1, t2) | (1ULL << (t3)))
#define CMDLINE_IMPL_TYPE_BIT_4(t1, t2, t3, t4)                                \
    (CMDLINE_IMPL_TYPE_BIT_3(t1, t2, t3) | (1ULL << (t4)))
#define CMDLINE_IMPL_TYPE_BIT_5(t1, t2, t3, t4, t5)                            \
    (CMDLINE_IMPL_TYPE_BIT_4(t1, t2, t3, t4) | (1ULL << (t5)))

/* ========== Type Enum ========== */

#define CMDLINE_STRICT_TYPE_ENUM(var)                                          \
    _Generic((var),                                                            \
        bool: TYPE_BOOL,                                                       \
        int8_t: TYPE_INT8,                                                     \
        uint8_t: TYPE_UINT8,                                                   \
        int16_t: TYPE_INT16,                                                   \
        uint16_t: TYPE_UINT16,                                                 \
        int32_t: TYPE_INT32,                                                   \
        uint32_t: TYPE_UINT32,                                                 \
        int64_t: TYPE_INT64,                                                   \
        uint64_t: TYPE_UINT64)

/* ========== Child Tuple Encodings ========== */

#define _CMDLINE_CHILD_KIND_POLY 1
#define _CMDLINE_CHILD_KIND_VAR 2

#define _CMDLINE_UNPACK(...) __VA_ARGS__
#define _CMDLINE_CHILD_SYM(parent_n, n)                                        \
    CONCAT(__cmdline_, CONCAT(parent_n, _##n))

#define _CMDLINE_CHILD_DECL_1(parent_n, n, _var, ...)                          \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    _CMDLINE_CHILD_SYM(parent_n, n) = {.name = #n,                             \
                                       .status = CMDLINE_ENTRY_NOT_FOUND,      \
                                       .parent = CMDLINE(parent_n),            \
                                       .types = 0,                             \
                                       .range = RANGE(1, 0),                   \
                                       .choices = NULL,                        \
                                       .mappings = NULL,                       \
                                       .flags_table = NULL,                    \
                                       .value.mode = CMDLINE_MODE_POLYMORPHIC, \
                                       .value.type = CMDLINE_TYPE_NONE,        \
                                       .flags = CMDLINE_ENTRY_FLAGS_NONE,      \
                                       ##__VA_ARGS__};

#define _CMDLINE_CHILD_DECL_2(parent_n, n, var, ...)                           \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    _CMDLINE_CHILD_SYM(parent_n, n) = {.name = #n,                             \
                                       .status = CMDLINE_ENTRY_NOT_FOUND,      \
                                       .parent = CMDLINE(parent_n),            \
                                       .types = 0,                             \
                                       .range = RANGE(1, 0),                   \
                                       .choices = NULL,                        \
                                       .mappings = NULL,                       \
                                       .flags_table = NULL,                    \
                                       .value.mode = CMDLINE_MODE_VAR,         \
                                       .value.write_to = &(var),               \
                                       .value.c_type = TYPE_TO_ENUM((var)),    \
                                       .value.parse = NULL,                    \
                                       .flags = CMDLINE_ENTRY_FLAGS_NONE,      \
                                       ##__VA_ARGS__};

#define _CMDLINE_CHILD_DECL_DISPATCH(parent_n, kind, n, var, ...)              \
    _CMDLINE_CHILD_DECL_##kind(parent_n, n, var, ##__VA_ARGS__)

#define _CMDLINE_CHILD_DECL_EXPAND(parent_n, ...)                              \
    _CMDLINE_CHILD_DECL_DISPATCH(parent_n, __VA_ARGS__)

#define _CMDLINE_CHILD_DECL_APPLY(parent_n, tuple)                             \
    _CMDLINE_CHILD_DECL_EXPAND(parent_n, _CMDLINE_UNPACK tuple)

/* ========== Variadic Children Map Expanders ========== */

#define _CMDLINE_CHILDREN_MAP_1(p, c1) _CMDLINE_CHILD_DECL_APPLY(p, c1)
#define _CMDLINE_CHILDREN_MAP_2(p, c1, c2)                                     \
    _CMDLINE_CHILD_DECL_APPLY(p, c1) _CMDLINE_CHILD_DECL_APPLY(p, c2)
#define _CMDLINE_CHILDREN_MAP_3(p, c1, c2, c3)                                 \
    _CMDLINE_CHILDREN_MAP_2(p, c1, c2) _CMDLINE_CHILD_DECL_APPLY(p, c3)
#define _CMDLINE_CHILDREN_MAP_4(p, c1, c2, c3, c4)                             \
    _CMDLINE_CHILDREN_MAP_3(p, c1, c2, c3) _CMDLINE_CHILD_DECL_APPLY(p, c4)
#define _CMDLINE_CHILDREN_MAP_5(p, c1, c2, c3, c4, c5)                         \
    _CMDLINE_CHILDREN_MAP_4(p, c1, c2, c3, c4) _CMDLINE_CHILD_DECL_APPLY(p, c5)
#define _CMDLINE_CHILDREN_MAP_6(p, c1, c2, c3, c4, c5, c6)                     \
    _CMDLINE_CHILDREN_MAP_5(p, c1, c2, c3, c4, c5)                             \
    _CMDLINE_CHILD_DECL_APPLY(p, c6)
#define _CMDLINE_CHILDREN_MAP_7(p, c1, c2, c3, c4, c5, c6, c7)                 \
    _CMDLINE_CHILDREN_MAP_6(p, c1, c2, c3, c4, c5, c6)                         \
    _CMDLINE_CHILD_DECL_APPLY(p, c7)
#define _CMDLINE_CHILDREN_MAP_8(p, c1, c2, c3, c4, c5, c6, c7, c8)             \
    _CMDLINE_CHILDREN_MAP_7(p, c1, c2, c3, c4, c5, c6, c7)                     \
    _CMDLINE_CHILD_DECL_APPLY(p, c8)
#define _CMDLINE_CHILDREN_MAP_9(p, c1, c2, c3, c4, c5, c6, c7, c8, c9)         \
    _CMDLINE_CHILDREN_MAP_8(p, c1, c2, c3, c4, c5, c6, c7, c8)                 \
    _CMDLINE_CHILD_DECL_APPLY(p, c9)
#define _CMDLINE_CHILDREN_MAP_10(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10)   \
    _CMDLINE_CHILDREN_MAP_9(p, c1, c2, c3, c4, c5, c6, c7, c8, c9)             \
    _CMDLINE_CHILD_DECL_APPLY(p, c10)
#define _CMDLINE_CHILDREN_MAP_11(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10,   \
                                 c11)                                          \
    _CMDLINE_CHILDREN_MAP_10(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10)       \
    _CMDLINE_CHILD_DECL_APPLY(p, c11)
#define _CMDLINE_CHILDREN_MAP_12(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10,   \
                                 c11, c12)                                     \
    _CMDLINE_CHILDREN_MAP_11(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11)  \
    _CMDLINE_CHILD_DECL_APPLY(p, c12)
#define _CMDLINE_CHILDREN_MAP_13(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10,   \
                                 c11, c12, c13)                                \
    _CMDLINE_CHILDREN_MAP_12(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11,  \
                             c12)                                              \
    _CMDLINE_CHILD_DECL_APPLY(p, c13)
#define _CMDLINE_CHILDREN_MAP_14(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10,   \
                                 c11, c12, c13, c14)                           \
    _CMDLINE_CHILDREN_MAP_13(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11,  \
                             c12, c13)                                         \
    _CMDLINE_CHILD_DECL_APPLY(p, c14)
#define _CMDLINE_CHILDREN_MAP_15(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10,   \
                                 c11, c12, c13, c14, c15)                      \
    _CMDLINE_CHILDREN_MAP_14(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11,  \
                             c12, c13, c14)                                    \
    _CMDLINE_CHILD_DECL_APPLY(p, c15)
#define _CMDLINE_CHILDREN_MAP_16(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10,   \
                                 c11, c12, c13, c14, c15, c16)                 \
    _CMDLINE_CHILDREN_MAP_15(p, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11,  \
                             c12, c13, c14, c15)                               \
    _CMDLINE_CHILD_DECL_APPLY(p, c16)

/* ========== Wrappers ========== */

#define CMDLINE_DECLARE_FX(n, var, ...)                                        \
    CMDLINE_DECLARE_VAR(n, var, .types = CMDLINE_TYPES(CMDLINE_TYPE_FX),       \
                        ##__VA_ARGS__)
#define CMDLINE_DECLARE_DURATION(n, var, ...)                                  \
    CMDLINE_DECLARE_VAR(n, var, .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION), \
                        ##__VA_ARGS__)
#define CMDLINE_DECLARE_SIZE(n, var, ...)                                      \
    CMDLINE_DECLARE_VAR(                                                       \
        n, var, .types = CMDLINE_TYPES(CMDLINE_TYPE_DATA_SIZE), ##__VA_ARGS__)
#define CMDLINE_DECLARE_CPUS(n, var, ...)                                      \
    CMDLINE_DECLARE_VAR(n, var, .types = CMDLINE_TYPES(CMDLINE_TYPE_CPU_MASK), \
                        ##__VA_ARGS__)
#define CMDLINE_DECLARE_STRING(n, var, ...)                                    \
    CMDLINE_DECLARE_VAR(n, var, .types = CMDLINE_TYPES(CMDLINE_TYPE_STRING),   \
                        ##__VA_ARGS__)

#define CMDLINE_CHILD_DECLARE_FX(parent_n, n, var, ...)                        \
    CMDLINE_CHILD_DECLARE_VAR(parent_n, n, var,                                \
                              .types = CMDLINE_TYPES(CMDLINE_TYPE_FX),         \
                              ##__VA_ARGS__)
#define CMDLINE_CHILD_DECLARE_DURATION(parent_n, n, var, ...)                  \
    CMDLINE_CHILD_DECLARE_VAR(parent_n, n, var,                                \
                              .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),   \
                              ##__VA_ARGS__)
#define CMDLINE_CHILD_DECLARE_SIZE(parent_n, n, var, ...)                      \
    CMDLINE_CHILD_DECLARE_VAR(parent_n, n, var,                                \
                              .types = CMDLINE_TYPES(CMDLINE_TYPE_DATA_SIZE),  \
                              ##__VA_ARGS__)
#define CMDLINE_CHILD_DECLARE_CPUS(parent_n, n, var, ...)                      \
    CMDLINE_CHILD_DECLARE_VAR(parent_n, n, var,                                \
                              .types = CMDLINE_TYPES(CMDLINE_TYPE_CPU_MASK),   \
                              ##__VA_ARGS__)
#define CMDLINE_CHILD_DECLARE_STRING(parent_n, n, var, ...)                    \
    CMDLINE_CHILD_DECLARE_VAR(parent_n, n, var,                                \
                              .types = CMDLINE_TYPES(CMDLINE_TYPE_STRING),     \
                              ##__VA_ARGS__)

#define CMDLINE_INNER_FX(n, var, ...)                                          \
    CMDLINE_INNER_VAR(n, var, .types = CMDLINE_TYPES(CMDLINE_TYPE_FX),         \
                      ##__VA_ARGS__)
#define CMDLINE_INNER_DURATION(n, var, ...)                                    \
    CMDLINE_INNER_VAR(n, var, .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),   \
                      ##__VA_ARGS__)
#define CMDLINE_INNER_SIZE(n, var, ...)                                        \
    CMDLINE_INNER_VAR(n, var, .types = CMDLINE_TYPES(CMDLINE_TYPE_DATA_SIZE),  \
                      ##__VA_ARGS__)
#define CMDLINE_INNER_CPUS(n, var, ...)                                        \
    CMDLINE_INNER_VAR(n, var, .types = CMDLINE_TYPES(CMDLINE_TYPE_CPU_MASK),   \
                      ##__VA_ARGS__)
#define CMDLINE_INNER_STRING(n, var, ...)                                      \
    CMDLINE_INNER_VAR(n, var, .types = CMDLINE_TYPES(CMDLINE_TYPE_STRING),     \
                      ##__VA_ARGS__)

#define CMDLINE_SCHEMA_PROP_FX(struct_type, member, ...)                       \
    CMDLINE_SCHEMA_PROP(struct_type, member,                                   \
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_FX),               \
                        ##__VA_ARGS__)
#define CMDLINE_SCHEMA_PROP_DURATION(struct_type, member, ...)                 \
    CMDLINE_SCHEMA_PROP(struct_type, member,                                   \
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),         \
                        ##__VA_ARGS__)
#define CMDLINE_SCHEMA_PROP_SIZE(struct_type, member, ...)                     \
    CMDLINE_SCHEMA_PROP(struct_type, member,                                   \
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DATA_SIZE),        \
                        ##__VA_ARGS__)
#define CMDLINE_SCHEMA_PROP_CPUS(struct_type, member, ...)                     \
    CMDLINE_SCHEMA_PROP(struct_type, member,                                   \
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_CPU_MASK),         \
                        ##__VA_ARGS__)
#define CMDLINE_SCHEMA_PROP_STRING(struct_type, member, ...)                   \
    CMDLINE_SCHEMA_PROP(struct_type, member,                                   \
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_STRING),           \
                        ##__VA_ARGS__)

enum errno cmdline_parse_bool(void *write_to, const char *text);
enum errno cmdline_parse_fx(void *write_to, const char *text);
enum errno cmdline_parse_duration(void *write_to, const char *text);
enum errno cmdline_parse_data_size(void *write_to, const char *text);
enum errno cmdline_parse_cpu_mask(void *write_to, const char *text);
enum errno cmdline_parse_mac(void *write_to, const char *text);
enum errno cmdline_parse_string(void *write_to, const char *text);

enum errno cmdline_extract_bool(struct cmdline_value *val, bool *out);
enum errno cmdline_extract_u64(struct cmdline_value *val, uint64_t *out);
enum errno cmdline_extract_i64(struct cmdline_value *val, int64_t *out);
enum errno cmdline_extract_u32(struct cmdline_value *val, uint32_t *out);
enum errno cmdline_extract_i32(struct cmdline_value *val, int32_t *out);
enum errno cmdline_extract_u16(struct cmdline_value *val, uint16_t *out);
enum errno cmdline_extract_i16(struct cmdline_value *val, int16_t *out);
enum errno cmdline_extract_u8(struct cmdline_value *val, uint8_t *out);
enum errno cmdline_extract_i8(struct cmdline_value *val, int8_t *out);
enum errno cmdline_extract_fx(struct cmdline_value *val, fx32_32_t *out);
enum errno cmdline_extract_duration(struct cmdline_value *val, time_ns_t *out);
enum errno cmdline_extract_mac(struct cmdline_value *val, uint64_t *out);
enum errno cmdline_extract_range(struct cmdline_value *val,
                                 struct cmdline_range *out);
enum errno cmdline_extract_cpu_mask(struct cmdline_value *val,
                                    struct cpu_mask *out);
enum errno cmdline_extract_string(struct cmdline_value *val, char **out);
enum errno cmdline_extract_const_string(struct cmdline_value *val,
                                        const char **out);
enum errno cmdline_extract_list(struct cmdline_value *val,
                                struct cmdline_list *out);
