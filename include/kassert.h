/* @title: Assertions */
#include <compiler.h>
#include <console/panic.h>

/*
 * Notes on kassert naming:
 *
 * no suffix - always fires, unhookable "the big panic"
 * _debug - fires on DEBUG_ASSERT, hookable "panic for debugging"
 * _oops - always fires, hookable "small invariant broken"
 *
 */

#define _kassert_pick(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, NAME, ...) \
    NAME

#define _kassert_dispatch(name, prefix, ...)                                   \
    _kassert_pick(__VA_ARGS__, name##_n, name##_n, name##_n, name##_n,         \
                  name##_n, name##_n, name##_n, name##_n, name##_n, name##_n,  \
                  name##_1)(prefix, __VA_ARGS__)

#define _kassert_debug_off_dispatch(first, ...) ({ first; })

#define _kassert_eval(prefix, x, msg_stmt)                                     \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof__(x), void),   \
                          ({ (x); }), ({                                       \
                              __typeof__(x) _kassert_res = (x);                \
                              if (unlikely(!(_kassert_res))) {                 \
                                  msg_stmt;                                    \
                                  __builtin_unreachable();                     \
                              }                                                \
                              _kassert_res;                                    \
                          }))

#define _kassert_1(prefix, x)                                                  \
    _kassert_eval(prefix, x, panic(prefix "Assertion \"" #x "\" failed"))

#define _kassert_n(prefix, x, fmt, ...)                                        \
    _kassert_eval(prefix, x,                                                   \
                  panic(prefix "Assertion \"" #x                               \
                               "\" failed with message: " fmt,                 \
                        ##__VA_ARGS__))

#define _kassert_oops_1(prefix, x)                                             \
    _kassert_eval(prefix, x, panic(prefix "Oops, assertion \"" #x "\" failed"))

#define _kassert_oops_n(prefix, x, fmt, ...)                                   \
    _kassert_eval(prefix, x,                                                   \
                  panic(prefix "Oops, assertion \"" #x                         \
                               "\" failed with message: " fmt,                 \
                        ##__VA_ARGS__))

#define _kassert_debug_1(prefix, x)                                            \
    _kassert_eval(prefix, x, panic(prefix "Debug assertion \"" #x "\" failed"))

#define _kassert_debug_n(prefix, x, fmt, ...)                                  \
    _kassert_eval(prefix, x,                                                   \
                  panic(prefix "Debug assertion \"" #x                         \
                               "\" failed with message: " fmt,                 \
                        ##__VA_ARGS__))

#define _kassert_fail(prefix, ...)                                             \
    do {                                                                       \
        panic(prefix __VA_ARGS__);                                             \
        __builtin_unreachable();                                               \
    } while (0)

#define kassert(...) _kassert_dispatch(_kassert, "", __VA_ARGS__)

#define kassert_unreachable(...) _kassert_fail("unreachable! ", ##__VA_ARGS__)
#define kassert_unimplemented(...)                                             \
    _kassert_fail("unimplemented! ", ##__VA_ARGS__)
#define kassert_todo(...) _kassert_fail("TODO: ", ##__VA_ARGS__)

#ifdef DEBUG_ASSERT

#define kassert_debug(...)                                                     \
    _kassert_dispatch(_kassert_debug, "DEBUG ", __VA_ARGS__)

#define kassert_debug_unreachable(...)                                         \
    _kassert_fail("DEBUG unreachable! ", ##__VA_ARGS__)
#define kassert_debug_unimplemented(...)                                       \
    _kassert_fail("DEBUG unimplemented! ", ##__VA_ARGS__)
#define kassert_debug_todo(...) _kassert_fail("DEBUG TODO: ", ##__VA_ARGS__)

#else

#define kassert_debug(...) _kassert_debug_off_dispatch(__VA_ARGS__)
#define kassert_debug_unreachable(...) _kassert_debug_off_dispatch(__VA_ARGS__)
#define kassert_debug_unimplemented(...)                                       \
    _kassert_debug_off_dispatch(__VA_ARGS__)
#define kassert_debug_todo(...) _kassert_debug_off_dispatch(__VA_ARGS__)

#endif
