/* @title: Enforcement */
#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <console/crash.h>
#include <console/panic.h>

#define _enforce_pick(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, NAME, ...) \
    NAME

#define _enforce_dispatch(default, name, prefix, ...)                          \
    _enforce_pick(__VA_ARGS__, name##_n, name##_n, name##_n, name##_n,         \
                  name##_n, name##_n, name##_n, name##_n, name##_n, name##_2,  \
                  name##_1)(default, prefix, __VA_ARGS__)

#define _enforce_as_code(x) ct_as_type(enum crash_code, x)
#define _enforce_msg(x) "Enforcement \"" #x "\" failed"

#define _debug_enforce_off_dispatch(first, ...)                                \
    _debug_enforce_off_as(first, PP_CONCAT(_enforce_res_, __COUNTER__))

#define _debug_enforce_off_as(first, res_)                                     \
    ({                                                                         \
        ct_decay(first) res_ = (first);                                        \
        if (cc_unlikely(!(res_)))                                              \
            ci_unreachable();                                                  \
                                                                               \
        (void) res_;                                                           \
    })

#define _enforce_eval(x, msg_stmt)                                             \
    _enforce_eval_as(x, msg_stmt, PP_CONCAT(_enforce_res_, __COUNTER__))

#define _enforce_eval_as(x, msg_stmt, res_)                                    \
    ({                                                                         \
        ct_decay(x) res_ = (x);                                                \
        if (cc_unlikely(!(res_))) {                                            \
            msg_stmt;                                                          \
            ci_unreachable();                                                  \
        }                                                                      \
        res_;                                                                  \
    })
/*
 * enforce(x)
 */
#define _enforce_1(default, prefix, x)                                         \
    _enforce_eval(x, enforce_impl_default(CRASH_CODE_TO_PAYLOAD(default),      \
                                          __FILE__, __LINE__, __func__,        \
                                          prefix, _enforce_msg(x), NULL))

/*
 * enforce(x, CODE)
 * enforce(x, "msg")
 */
#define _enforce_2(default, prefix, x, a)                                      \
    _enforce_eval(                                                             \
        x, ci_choose_expr(                                                     \
               ct_is_str(a),                                                   \
               enforce_impl_default(CRASH_CODE_TO_PAYLOAD(default), __FILE__,  \
                                    __LINE__, __func__, prefix,                \
                                    _enforce_msg(x), ct_as_str(a)),            \
               enforce_impl_default(                                           \
                   CRASH_CODE_TO_PAYLOAD(_enforce_as_code(a)), __FILE__,       \
                   __LINE__, __func__, prefix, _enforce_msg(x), NULL)))

/*
 * enforce(x, "msg %d", 12)
 * enforce(x, CODE, "msg %d", 12)
 */
#define _enforce_n(default, prefix, x, a, b, ...)                              \
    _enforce_eval(x, ci_choose_expr(                                           \
                         ct_is_str(a), /* a is format, b is first vararg */    \
                         enforce_impl_default(CRASH_CODE_TO_PAYLOAD(default),  \
                                              __FILE__, __LINE__, __func__,    \
                                              prefix, _enforce_msg(x),         \
                                              ct_as_str(a), b, ##__VA_ARGS__), \
                         enforce_impl_default(                                 \
                             CRASH_CODE_TO_PAYLOAD(_enforce_as_code(a)),       \
                             __FILE__, __LINE__, __func__, prefix,             \
                             _enforce_msg(x), ct_as_str(b), ##__VA_ARGS__)))

#define _enforce_fail(c, prefix, ...)                                          \
    enforce_impl_default(CRASH_CODE_TO_PAYLOAD(c), __FILE__, __LINE__,         \
                         __func__, prefix, NULL, "" __VA_ARGS__)

#define enforce_with_default(default, ...)                                     \
    _enforce_dispatch(default, "", __VA_ARGS__)
#define enforce(...)                                                           \
    _enforce_dispatch(CRASH_CODE_GENERIC, _enforce, "", __VA_ARGS__)

#define enforce_with(x, payload, fmt, ...)                                     \
    ({                                                                         \
        ct_decay(x) _enforce_res = (x);                                        \
        if (cc_unlikely(!(_enforce_res))) {                                    \
            enforce_impl_default(payload, __FILE__, __LINE__, __func__, "",    \
                                 _enforce_msg(x), fmt, ##__VA_ARGS__);         \
            ci_unreachable();                                                  \
        }                                                                      \
        _enforce_res;                                                          \
    })

#define unimplemented(...)                                                     \
    _enforce_fail(CRASH_CODE_GENERIC, "unimplemented! ", ##__VA_ARGS__)

#define todo(...) _enforce_fail(CRASH_CODE_GENERIC, "TODO: ", ##__VA_ARGS__)

#define unreachable(...)                                                       \
    _enforce_fail(CRASH_CODE_GENERIC, "unreachable! ", ##__VA_ARGS__)

#ifdef DEBUG_ENFORCE

#define debug_enforce_with_default(default, ...)                               \
    _enforce_dispatch(default, _enforce, "DEBUG ", __VA_ARGS__)

#define debug_enforce(...)                                                     \
    _enforce_dispatch(CRASH_CODE_GENERIC, _enforce, "DEBUG ", __VA_ARGS__)

#else

#define debug_enforce(...) _debug_enforce_off_dispatch(__VA_ARGS__)

#endif
