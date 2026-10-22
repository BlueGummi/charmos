/* @title: Kernel Panic Interface */
#pragma once
#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <console/crash.h>

cc_noreturn cc_cold cc_printf_like(4, 5) cc_nonnull(1, 3, 4) void
panic_impl_msg(const char *file, int line, const char *func, const char *fmt,
               ...);
cc_noreturn cc_cold cc_printf_like(5, 6) cc_nonnull(1, 3, 5) void
panic_impl_code(const char *file, int line, const char *func,
                enum crash_code code, const char *fmt, ...);
cc_noreturn cc_cold cc_nonnull(1, 3) void
panic_impl_code_only(const char *file, int line, const char *func,
                     enum crash_code code);

#define _panic_pick(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12,     \
                    _13, _14, _15, _16, NAME, ...)                             \
    NAME

#define _panic_arity(...)                                                      \
    _panic_pick(_, ##__VA_ARGS__, _panic_n, _panic_n, _panic_n, _panic_n,      \
                _panic_n, _panic_n, _panic_n, _panic_n, _panic_n, _panic_n,    \
                _panic_n, _panic_n, _panic_n, _panic_n, _panic_n, _panic_1,    \
                _panic_0)

#define _panic_fn(a, code_fn)                                                  \
    cc_generic((a), char * : panic_impl_msg, const char * : panic_impl_msg,    \
               default : code_fn)

/* panic() */
#define _panic_0()                                                             \
    panic_impl_code_only(__FILE__, __LINE__, __func__, CRASH_CODE_GENERIC)

/*
 * panic(CODE)
 * panic("msg")
 */
#define _panic_1(a)                                                            \
    _panic_fn(a, panic_impl_code_only)(__FILE__, __LINE__, __func__, a)

/*
 * panic("msg %d", 12)
 * panic(CODE, "msg %d", 12)
 */
#define _panic_n(a, ...)                                                       \
    _panic_fn(a, panic_impl_code)(__FILE__, __LINE__, __func__, a, __VA_ARGS__)

#define panic(...) _panic_arity(__VA_ARGS__)(__VA_ARGS__)
