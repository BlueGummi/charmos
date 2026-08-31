/* @title: Kernel Panic Interface */
#pragma once
#include <console/crash.h>

__noreturn void panic_impl_default(struct crash_payload payload,
                                   const char *file, int line, const char *func,
                                   const char *fmt, ...);

#define _panic_pick(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, NAME, ...)   \
    NAME

#define _panic_dispatch(default, name, ...)                                    \
    _panic_pick(__VA_ARGS__, name##_n, name##_n, name##_n, name##_n, name##_n, \
                name##_n, name##_n, name##_n, name##_n, name##_2,              \
                name##_1)(default, __VA_ARGS__)

#define _panic_as_code(x) __comptime_as_type(enum crash_code, x)

/*
 * panic(CODE)
 * panic("msg")
 */
#define _panic_1(default, x)                                                   \
    __builtin_choose_expr(                                                     \
        __comptime_is_str(x),                                                  \
        panic_impl_default(CRASH_CODE_TO_PAYLOAD(default), __FILE__, __LINE__, \
                           __func__, __comptime_as_str(x)),                    \
        panic_impl_default(CRASH_CODE_TO_PAYLOAD(_panic_as_code(x)), __FILE__, \
                           __LINE__, __func__, "No message supplied"))
/*
 * panic(CODE, "msg")
 * panic("msg %d", 12)
 */
#define _panic_2(default, x, y)                                                \
    __builtin_choose_expr(                                                     \
        __comptime_is_str(x),                                                  \
        panic_impl_default(CRASH_CODE_TO_PAYLOAD(default), __FILE__, __LINE__, \
                           __func__, __comptime_as_str(x),                     \
                           y), /* x is format, y is first vararg */            \
        panic_impl_default(CRASH_CODE_TO_PAYLOAD(_panic_as_code(x)), __FILE__, \
                           __LINE__, __func__, __comptime_as_str(y)))

/*
 * panic(CODE, "msg %d", 12)
 * panic("msg %d %d", 12, 13)
 */
#define _panic_n(default, x, y, ...)                                           \
    __builtin_choose_expr(                                                     \
        __comptime_is_str(x),                                                  \
        panic_impl_default(CRASH_CODE_TO_PAYLOAD(default), __FILE__, __LINE__, \
                           __func__, __comptime_as_str(x), y, ##__VA_ARGS__),  \
        panic_impl_default(CRASH_CODE_TO_PAYLOAD(_panic_as_code(x)), __FILE__, \
                           __LINE__, __func__, __comptime_as_str(y),           \
                           ##__VA_ARGS__))

#define panic(...) _panic_dispatch(CRASH_CODE_GENERIC, _panic, "", __VA_ARGS__)
#define panic_with(payload, fmt, ...)                                          \
    panic_impl_default(payload, __FILE__, __LINE__, __func__, fmt,             \
                       ##__VA_ARGS__)

#define panic_with_default(default, ...)                                       \
    _panic_dispatch(default, _panic, "", __VA_ARGS__)
