/* @title: Test Export */
#pragma once

/* TODO: (qol) maybe I should make include/test/
 * and move this under with test.h */

#ifdef TEST_ENABLED
#define TEST_EXPORT_AS(sym_name, fn)                                           \
    extern typeof(fn) __test_sym_##sym_name __attribute__((alias(#fn), used))
#define TEST_EXPORT(fn) TEST_EXPORT_AS(fn, fn)

#define TEST_IMPORT(ret, fn, ...) ret __test_sym_##fn(__VA_ARGS__)

#define TEST_CALL(fn) __test_sym_##fn
#else
#define TEST_EXPORT(fn)
#define TEST_IMPORT(ret, fn, ...)
#define TEST_CALL(fn) (void)
#endif
