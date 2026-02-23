/* @title: Compiler Functions */
#pragma once

#define __noinline __attribute__((noinline))

#define __noreturn __attribute__((noreturn))

#define __unused __attribute__((unused))

#define __warn_unused_result __attribute__((warn_unused_result))

#define __packed __attribute__((__packed__))

#define __aligned(x) __attribute__((aligned(x)))

#define __cache_aligned __attribute__((aligned(64)))

#define __linker_aligned __attribute__((aligned(64)))

#define __used __attribute__((used))

#define __section(x) __attribute__((section(x)))

#define __hidden __attribute__((visibility("hidden")))
#define __export __attribute__((visibility("default")))

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define __no_sanitize_address __attribute__((no_sanitize("address")))

#define __deprecated __attribute__((deprecated))
#define __deprecated_msg(msg) __attribute__((deprecated(msg)))

#define __pure __attribute__((pure))

#define __constfn __attribute__((const))

#define __noclone __attribute__((noclone))

#define __malloc_like __attribute__((malloc))

#define __printf_like(fmt_idx, arg_idx)                                        \
    __attribute__((format(printf, fmt_idx, arg_idx)))

#define __constructor(prio) __attribute__((constructor(prio)))
#define __destructor(prio) __attribute__((destructor(prio)))

#define __fallthrough __attribute__((fallthrough))

#if defined(__GNUC__)
#define __restrict __restrict__
#else
#define __restrict
#endif

#define static_assert_struct_size_eq(__struct, __want)                         \
    _Static_assert(sizeof(struct __struct) == __want,                          \
                   "size of struct" #__struct                                  \
                   " does not match expected size " #__want)

#define static_assert(a, b) _Static_assert(a, b)

#define smp_mb() atomic_thread_fence(memory_order_seq_cst)
#define smp_rmb() atomic_thread_fence(memory_order_acquire)
#define smp_wmb() atomic_thread_fence(memory_order_release)

#define PP_NARG(...) PP_NARG_(__VA_ARGS__, PP_RSEQ_N())
#define PP_NARG_(...) PP_ARG_N(__VA_ARGS__)
#define PP_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14,  \
                 _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26,   \
                 _27, _28, _29, _30, _31, _32, _33, _34, _35, _36, _37, _38,   \
                 _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, _50,   \
                 _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, _61, _62,   \
                 _63, N, ...)                                                  \
    N
#define PP_RSEQ_N()                                                            \
    63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46,    \
        45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29,    \
        28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12,    \
        11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0
