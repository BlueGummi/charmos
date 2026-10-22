/* @title: Enforcement */
#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <console/crash.h>
#include <console/panic.h>

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

#define _enforce_pick(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12,   \
                      _13, _14, _15, _16, NAME, ...)                           \
    NAME

#define _enforce_arity(...)                                                    \
    _enforce_pick(_, ##__VA_ARGS__, _enforce_n, _enforce_n, _enforce_n,        \
                  _enforce_n, _enforce_n, _enforce_n, _enforce_n, _enforce_n,  \
                  _enforce_n, _enforce_n, _enforce_n, _enforce_n, _enforce_n,  \
                  _enforce_n, _enforce_n, _enforce_1, _enforce_0)

#define _enforce_fn(a, code_fn)                                                \
    cc_generic((a), char * : enforce_impl_msg,                                 \
               const char * : enforce_impl_msg, default : code_fn)

#define _enforce_ctx(prefix, x)                                                \
    __FILE__, __LINE__, __func__, prefix, _enforce_msg(x)

/* enforce(x) */
#define _enforce_0(prefix, x)                                                  \
    _enforce_eval(x, enforce_impl_code_only(_enforce_ctx(prefix, x),           \
                                            CRASH_CODE_GENERIC))

/*
 * enforce(x, CODE)
 * enforce(x, "msg")
 */
#define _enforce_1(prefix, x, a)                                               \
    _enforce_eval(                                                             \
        x, _enforce_fn(a, enforce_impl_code_only)(_enforce_ctx(prefix, x), a))

/*
 * enforce(x, "msg %d", 12)
 * enforce(x, CODE, "msg %d", 12)
 */
#define _enforce_n(prefix, x, a, ...)                                          \
    _enforce_eval(x, _enforce_fn(a, enforce_impl_code)(                        \
                         _enforce_ctx(prefix, x), a, __VA_ARGS__))

#define _enforce_fail(prefix, ...)                                             \
    enforce_impl_msg(__FILE__, __LINE__, __func__, prefix, NULL, "" __VA_ARGS__)

#define enforce(x, ...) _enforce_arity(__VA_ARGS__)("", x, ##__VA_ARGS__)

#define unimplemented(...) _enforce_fail("unimplemented! ", ##__VA_ARGS__)

#define todo(...) _enforce_fail("TODO: ", ##__VA_ARGS__)

#define unreachable(...) _enforce_fail("unreachable! ", ##__VA_ARGS__)

#ifdef DEBUG_ENFORCE

#define debug_enforce(x, ...)                                                  \
    _enforce_arity(__VA_ARGS__)("DEBUG ", x, ##__VA_ARGS__)

#else

#define debug_enforce(...) _debug_enforce_off_dispatch(__VA_ARGS__)

#endif
