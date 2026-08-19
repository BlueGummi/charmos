/* @title: Test Export */
#pragma once
#include <compiler.h>
#include <linker/symbols.h>
#include <stddef.h>

#ifdef TEST_ENABLED

#define TEST_EXPORT_AS(sym_name, fn)                                           \
    extern typeof(fn) __test_sym_##sym_name __attribute__((alias(#fn), used)); \
    static LINKER_SECTION_OBJECT(const struct test_export_entry, test_exports) \
        __test_exp_##sym_name = {                                              \
            .name = #sym_name,                                                 \
            .fn_ptr = (void *) (fn),                                           \
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
    static ret (*__test_unsafe_fn_##sym_name)(__VA_ARGS__) = NULL;             \
    static LINKER_SECTION_OBJECT(const struct test_import_entry, test_imports) \
        __test_imp_##sym_name = {                                              \
            .name = #sym_name,                                                 \
            .target_fn_ptr = (void **) &__test_unsafe_fn_##sym_name,           \
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
#define TEST_CALL_UNSAFE(sym_name) __test_unsafe_fn_##sym_name

#else

#define TEST_EXPORT(fn)
#define TEST_IMPORT(ret, fn, ...)
#define TEST_IMPORT_UNSAFE(ret, sym_name, ...)
#define TEST_CALL(fn) (void)
#define TEST_CALL_UNSAFE(sym_name) (void)

#endif /* TEST_ENABLED */

struct test_export_entry {
    const char *name;
    void *fn_ptr;
};

struct test_import_entry {
    const char *name;
    void **target_fn_ptr;
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

LINKER_SECTION_DEFINE(struct test_import_entry, test_imports);
LINKER_SECTION_DEFINE(struct test_export_entry, test_exports);
LINKER_SECTION_DEFINE(struct test_signature_record, test_canonical_signatures);
LINKER_SECTION_DEFINE(struct test_signature_record, test_unsafe_signatures);
