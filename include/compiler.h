#pragma once

#define __always_inline inline __attribute__((always_inline))

#define __noinline __attribute__((noinline))

#define __noreturn __attribute__((noreturn))

#define __unused __attribute__((unused))

#define __warn_unused_result __attribute__((warn_unused_result))

#define __packed __attribute__((__packed__))

#define __aligned(x) __attribute__((aligned(x)))

#define __cache_aligned __attribute__((aligned(64)))

#define __used __attribute__((used))

#define __section(x) __attribute__((section(x)))

#define __hidden __attribute__((visibility("hidden")))
#define __export __attribute__((visibility("default")))

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

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
