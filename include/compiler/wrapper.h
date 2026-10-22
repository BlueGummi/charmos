/* @title: Type-Generic Compiler Wrappers */
#pragma once
#include <compiler/core.h>
#include <compiler/intrinsic.h>

#define cw_alloc_0() cc_malloc_like cc_warn_unused_result
#define cw_alloc_1(sz) cw_alloc_0() cc_alloc_size(sz)
#define cw_alloc_2(sz, align) cw_alloc_1(sz) cc_alloc_align(align)

#define cw_alloc(...) PP_CALL2(cw_alloc, __VA_ARGS__)

#define cw_popcount(x)                                                         \
    cc_generic((x), unsigned char : ci_popcount((unsigned int) (x)),           \
               signed char : ci_popcount((unsigned int) (unsigned char) (x)),  \
               char : ci_popcount((unsigned int) (unsigned char) (x)),         \
               unsigned short : ci_popcount((unsigned int) (x)),               \
               short : ci_popcount((unsigned int) (unsigned short) (x)),       \
               unsigned int : ci_popcount(x),                                  \
               int : ci_popcount((unsigned int) (x)),                          \
               unsigned long : ci_popcountl(x),                                \
               long : ci_popcountl((unsigned long) (x)),                       \
               unsigned long long : ci_popcountll(x),                          \
               long long : ci_popcountll((unsigned long long) (x)),            \
               default : ci_popcountll((unsigned long long) (x)))

#define cw_bswap16(x) ci_bswap16(x)
#define cw_bswap32(x) ci_bswap32(x)
#define cw_bswap64(x) ci_bswap64(x)

#define cw_bswap(x)                                                            \
    cc_generic((x), uint16_t : ci_bswap16(x),                                  \
               int16_t : (int16_t) ci_bswap16((uint16_t) (x)),                 \
               uint32_t : ci_bswap32(x),                                       \
               int32_t : (int32_t) ci_bswap32((uint32_t) (x)),                 \
               uint64_t : ci_bswap64(x),                                       \
               int64_t : (int64_t) ci_bswap64((uint64_t) (x)),                 \
               default : (sizeof(x) == 2   ? ci_bswap16((uint16_t) (x))        \
                          : sizeof(x) == 4 ? ci_bswap32((uint32_t) (x))        \
                          : sizeof(x) == 8 ? ci_bswap64((uint64_t) (x))        \
                                           : (x)))

/*
 * Count Leading Zeros
 */
#define cw_clz(x)                                                              \
    ({                                                                         \
        __auto_type __cw_clz_x = (x);                                          \
        ct_typecheck_unsigned_as(__cw_clz_x, x);                               \
        __cw_clz_x == 0 ? (unsigned int) ct_bitsizeof(__cw_clz_x)              \
        : sizeof(__cw_clz_x) <= sizeof(unsigned int)                           \
            ? (unsigned int) (ci_clz((unsigned int) __cw_clz_x) -              \
                              (ct_bitsizeof(unsigned int) -                    \
                               ct_bitsizeof(__cw_clz_x)))                      \
        : sizeof(__cw_clz_x) <= sizeof(unsigned long)                          \
            ? (unsigned int) (ci_clzl((unsigned long) __cw_clz_x) -            \
                              (ct_bitsizeof(unsigned long) -                   \
                               ct_bitsizeof(__cw_clz_x)))                      \
            : (unsigned int) ci_clzll((unsigned long long) __cw_clz_x);        \
    })

/*
 * Count Leading Zeros
 */
#define cw_clz_nonzero(x)                                                      \
    ({                                                                         \
        __auto_type __cw_clzn_x = (x);                                         \
        ct_typecheck_unsigned_as(__cw_clzn_x, x);                              \
        sizeof(__cw_clzn_x) <= sizeof(unsigned int)                            \
            ? (unsigned int) (ci_clz((unsigned int) __cw_clzn_x) -             \
                              (ct_bitsizeof(unsigned int) -                    \
                               ct_bitsizeof(__cw_clzn_x)))                     \
        : sizeof(__cw_clzn_x) <= sizeof(unsigned long)                         \
            ? (unsigned int) (ci_clzl((unsigned long) __cw_clzn_x) -           \
                              (ct_bitsizeof(unsigned long) -                   \
                               ct_bitsizeof(__cw_clzn_x)))                     \
            : (unsigned int) ci_clzll((unsigned long long) __cw_clzn_x);       \
    })

/*
 * Count Trailing Zeros
 */
#define cw_ctz(x)                                                              \
    ({                                                                         \
        __auto_type __cw_ctz_x = (x);                                          \
        ct_typecheck_unsigned_as(__cw_ctz_x, x);                               \
        __cw_ctz_x == 0                                                        \
            ? (unsigned int) ct_bitsizeof(__cw_ctz_x)                          \
            : (sizeof(__cw_ctz_x) <= sizeof(unsigned int)                      \
                   ? (unsigned int) ci_ctz((unsigned int) __cw_ctz_x)          \
               : sizeof(__cw_ctz_x) <= sizeof(unsigned long)                   \
                   ? (unsigned int) ci_ctzl((unsigned long) __cw_ctz_x)        \
                   : (unsigned int) ci_ctzll(                                  \
                         (unsigned long long) __cw_ctz_x));                    \
    })

/* Fast Count Trailing Zeros: requires x != 0 */
#define cw_ctz_nonzero(x)                                                      \
    ({                                                                         \
        __auto_type __cw_ctzn_x = (x);                                         \
        ct_typecheck_unsigned_as(__cw_ctzn_x, x);                              \
        sizeof(__cw_ctzn_x) <= sizeof(unsigned int)                            \
            ? (unsigned int) ci_ctz((unsigned int) __cw_ctzn_x)                \
        : sizeof(__cw_ctzn_x) <= sizeof(unsigned long)                         \
            ? (unsigned int) ci_ctzl((unsigned long) __cw_ctzn_x)              \
            : (unsigned int) ci_ctzll((unsigned long long) __cw_ctzn_x);       \
    })

/* Find First Set (1-based index, 0 if none) */
#define cw_ffs(x)                                                              \
    ({                                                                         \
        __auto_type __cw_ffs_x = (x);                                          \
        ct_typecheck_integer_as(__cw_ffs_x, x);                                \
        sizeof(__cw_ffs_x) <= sizeof(unsigned int)                             \
            ? (unsigned int) ci_ffs((int) __cw_ffs_x)                          \
        : sizeof(__cw_ffs_x) <= sizeof(unsigned long)                          \
            ? (unsigned int) ci_ffsl((long) __cw_ffs_x)                        \
            : (unsigned int) ci_ffsll((long long) __cw_ffs_x);                 \
    })

/* Find Last Set (1-based index, 0 if none) */
#define cw_fls(x)                                                              \
    ({                                                                         \
        __auto_type __cw_fls_x = (x);                                          \
        ct_typecheck_unsigned_as(__cw_fls_x, x);                               \
        __cw_fls_x == 0 ? 0u                                                   \
                        : (unsigned int) (ct_bitsizeof(__cw_fls_x) -           \
                                          cw_clz_nonzero(__cw_fls_x));         \
    })

#define cw_parity(x)                                                           \
    cc_generic(                                                                \
        (x), unsigned char : ci_parity((unsigned int) (x)),                    \
        signed char : ci_parity((unsigned int) (unsigned char) (x)),           \
        char : ci_parity((unsigned int) (unsigned char) (x)),                  \
        unsigned short : ci_parity((unsigned int) (x)),                        \
        short : ci_parity((unsigned int) (unsigned short) (x)),                \
        unsigned int : ci_parity(x), int : ci_parity((unsigned int) (x)),      \
        unsigned long : ci_parityl(x), long : ci_parityl((unsigned long) (x)), \
        unsigned long long : ci_parityll(x),                                   \
        long long : ci_parityll((unsigned long long) (x)),                     \
        default : ci_parityll((unsigned long long) (x)))

#define cw_rol(x, n)                                                           \
    ({                                                                         \
        __auto_type __cw_rol_x = (x);                                          \
        __auto_type __cw_rol_n = (n);                                          \
        ct_typecheck_unsigned_as(__cw_rol_x, x);                               \
        ct_typecheck_integer_as(__cw_rol_n, n);                                \
        unsigned int __cw_rol_shift = (unsigned int) __cw_rol_n;               \
        unsigned int __cw_rol_w = (unsigned int) ct_bitsizeof(__cw_rol_x);     \
        __cw_rol_shift &= (__cw_rol_w - 1u);                                   \
        (typeof(__cw_rol_x)) ((__cw_rol_x << __cw_rol_shift) |                 \
                              (__cw_rol_x >> ((__cw_rol_w - __cw_rol_shift) &  \
                                              (__cw_rol_w - 1u))));            \
    })

#define cw_ror(x, n)                                                           \
    ({                                                                         \
        __auto_type __cw_ror_x = (x);                                          \
        __auto_type __cw_ror_n = (n);                                          \
        ct_typecheck_unsigned_as(__cw_ror_x, x);                               \
        ct_typecheck_integer_as(__cw_ror_n, n);                                \
        unsigned int __cw_ror_shift = (unsigned int) __cw_ror_n;               \
        unsigned int __cw_ror_w = (unsigned int) ct_bitsizeof(__cw_ror_x);     \
        __cw_ror_shift &= (__cw_ror_w - 1u);                                   \
        (typeof(__cw_ror_x)) ((__cw_ror_x >> __cw_ror_shift) |                 \
                              (__cw_ror_x << ((__cw_ror_w - __cw_ror_shift) &  \
                                              (__cw_ror_w - 1u))));            \
    })

#define cw_sat_add(a, b)                                                       \
    ({                                                                         \
        __auto_type __cw_sa_a = (a);                                           \
        __auto_type __cw_sa_b = (b);                                           \
        typeof(__cw_sa_a) __cw_sa_res;                                         \
        if (ci_add_overflow(__cw_sa_a, __cw_sa_b, &__cw_sa_res)) {             \
            __cw_sa_res = (__cw_sa_b > 0) ? ct_max_val(typeof(__cw_sa_a))      \
                                          : ct_min_val(typeof(__cw_sa_a));     \
        }                                                                      \
        __cw_sa_res;                                                           \
    })

#define cw_sat_sub(a, b)                                                       \
    ({                                                                         \
        __auto_type __cw_ss_a = (a);                                           \
        __auto_type __cw_ss_b = (b);                                           \
        typeof(__cw_ss_a) __cw_ss_res;                                         \
        if (ci_sub_overflow(__cw_ss_a, __cw_ss_b, &__cw_ss_res)) {             \
            __cw_ss_res = (__cw_ss_b > 0) ? ct_min_val(typeof(__cw_ss_a))      \
                                          : ct_max_val(typeof(__cw_ss_a));     \
        }                                                                      \
        __cw_ss_res;                                                           \
    })
