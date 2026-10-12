/* @title: Test Export */
#pragma once
#include <compiler/core.h>
#include <linker/symbols.h>
#include <stddef.h>
#include <types/types.h>

#ifdef TEST_ENABLED

#define TEST_EXPORT_AS(sym_name, export_fn)                                    \
    extern typeof(export_fn) __test_sym_##sym_name cc_alias(export_fn)         \
    cc_used;                                                                   \
    static LINKER_SECTION_OBJECT(const struct test_export_entry, test_exports) \
        __test_exp_##sym_name = {                                              \
            .name = #sym_name,                                                 \
            .fn = any_fn_from(export_fn),                                      \
    }
#define TEST_EXPORT(fn) TEST_EXPORT_AS(fn, fn)

#define TEST_IMPORT(ret, fn, ...)                                              \
    ret __test_sym_##fn(__VA_ARGS__);                                          \
    static LINKER_SECTION_OBJECT(const struct test_signature_record,           \
                                 test_canonical_signatures)                    \
        __test_sig_##fn = {                                                    \
            .name = #fn,                                                       \
            .ret_str = #ret,                                                   \
            .args_str = #__VA_ARGS__,                                          \
            .file = __FILE__,                                                  \
            .line = __LINE__,                                                  \
    }

#define TEST_IMPORT_UNSAFE(ret, sym_name, ...)                                 \
    typedef ret (*__test_unsafe_fn_t_##sym_name)(__VA_ARGS__);                 \
    static any_fn_t __test_unsafe_fn_##sym_name;                               \
    static LINKER_SECTION_OBJECT(const struct test_import_entry, test_imports) \
        __test_imp_##sym_name = {                                              \
            .name = #sym_name,                                                 \
            .target_fn = &__test_unsafe_fn_##sym_name,                         \
            .import_file = __FILE__,                                           \
            .import_line = __LINE__,                                           \
    };                                                                         \
    static LINKER_SECTION_OBJECT(const struct test_signature_record,           \
                                 test_unsafe_signatures)                       \
        __test_unsig_##sym_name = {                                            \
            .name = #sym_name,                                                 \
            .ret_str = #ret,                                                   \
            .args_str = #__VA_ARGS__,                                          \
            .file = __FILE__,                                                  \
            .line = __LINE__,                                                  \
    }

#define TEST_CALL(fn) __test_sym_##fn
#define TEST_CALL_UNSAFE(sym_name)                                             \
    any_fn_to(__test_unsafe_fn_##sym_name, __test_unsafe_fn_t_##sym_name)

#else

#define TEST_EXPORT(fn)
#define TEST_IMPORT(ret, fn, ...)
#define TEST_IMPORT_UNSAFE(ret, sym_name, ...)
#define TEST_CALL(fn) (void)
#define TEST_CALL_UNSAFE(sym_name) (void)

#endif /* TEST_ENABLED */

struct test_export_entry {
    const char *name;
    any_fn_t fn;
};

struct test_import_entry {
    const char *name;
    any_fn_t *target_fn;
    const char *import_file;
    uint32_t import_line;
};

struct test_signature_record {
    const char *name;
    const char *ret_str;
    const char *args_str;
    const char *file;
    uint32_t line;
};

LINKER_SECTION_EXTERN(struct test_import_entry, test_imports);
LINKER_SECTION_EXTERN(struct test_export_entry, test_exports);
LINKER_SECTION_EXTERN(struct test_signature_record, test_canonical_signatures);
LINKER_SECTION_EXTERN(struct test_signature_record, test_unsafe_signatures);
