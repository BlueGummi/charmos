/* @title: Min, Max, Clamping, and Absolute Value */
#pragma once
#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <enforce.h>
#include <stdbool.h>

#define abs(N)                                                                 \
    ({                                                                         \
        __auto_type __abs_n = (N);                                             \
        ct_typecheck_signed_as(__abs_n, N);                                    \
        typeof(__abs_n) __abs_result = __abs_n;                                \
        if (__abs_n < 0) {                                                     \
            bool __abs_overflow =                                              \
                ci_sub_overflow((typeof(__abs_n)) 0, __abs_n, &__abs_result);  \
            (void) enforce(!__abs_overflow);                                   \
        }                                                                      \
        __abs_result;                                                          \
    })

#define CLAMP(val_, min_, max_)                                                \
    ({                                                                         \
        __auto_type __cl_val = (val_);                                         \
        __auto_type __cl_lo = (min_);                                          \
        __auto_type __cl_hi = (max_);                                          \
        typedef ct_common_type_2(__cl_val, __cl_lo) __cl_t1;                   \
        typedef ct_common_type_2((__cl_t1) 0, __cl_hi) __cl_t;                 \
        ct_typecheck_widenable_to((__cl_t) 0, val_);                           \
        ct_typecheck_widenable_to((__cl_t) 0, min_);                           \
        ct_typecheck_widenable_to((__cl_t) 0, max_);                           \
        __cl_t __cl_v = (__cl_t) __cl_val;                                     \
        __cl_t __cl_l = (__cl_t) __cl_lo;                                      \
        __cl_t __cl_h = (__cl_t) __cl_hi;                                      \
        (void) enforce(__cl_l <= __cl_h);                                      \
        if (__cl_v > __cl_h)                                                   \
            __cl_v = __cl_h;                                                   \
        if (__cl_v < __cl_l)                                                   \
            __cl_v = __cl_l;                                                   \
        (__cl_t) __cl_v;                                                       \
    })

#define _MIN_1(a) (a)

#define _MIN_2(a, b)                                                           \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef __mm_t2 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 < __mm_r ? __mm_c2 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MIN_3(a, b, c)                                                        \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef __mm_t3 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 < __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 < __mm_r ? __mm_c3 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MIN_4(a, b, c, d)                                                     \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        __auto_type __mm_4 = (d);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef ct_common_type_2((__mm_t3) 0, __mm_4) __mm_t4;                 \
        typedef __mm_t4 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        ct_typecheck_widenable_to((__mm_t) 0, d);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 < __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 < __mm_r ? __mm_c3 : __mm_r;                          \
        __mm_t __mm_c4 = (__mm_t) __mm_4;                                      \
        __mm_r = __mm_c4 < __mm_r ? __mm_c4 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MIN_5(a, b, c, d, e)                                                  \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        __auto_type __mm_4 = (d);                                              \
        __auto_type __mm_5 = (e);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef ct_common_type_2((__mm_t3) 0, __mm_4) __mm_t4;                 \
        typedef ct_common_type_2((__mm_t4) 0, __mm_5) __mm_t5;                 \
        typedef __mm_t5 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        ct_typecheck_widenable_to((__mm_t) 0, d);                              \
        ct_typecheck_widenable_to((__mm_t) 0, e);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 < __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 < __mm_r ? __mm_c3 : __mm_r;                          \
        __mm_t __mm_c4 = (__mm_t) __mm_4;                                      \
        __mm_r = __mm_c4 < __mm_r ? __mm_c4 : __mm_r;                          \
        __mm_t __mm_c5 = (__mm_t) __mm_5;                                      \
        __mm_r = __mm_c5 < __mm_r ? __mm_c5 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MIN_6(a, b, c, d, e, f)                                               \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        __auto_type __mm_4 = (d);                                              \
        __auto_type __mm_5 = (e);                                              \
        __auto_type __mm_6 = (f);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef ct_common_type_2((__mm_t3) 0, __mm_4) __mm_t4;                 \
        typedef ct_common_type_2((__mm_t4) 0, __mm_5) __mm_t5;                 \
        typedef ct_common_type_2((__mm_t5) 0, __mm_6) __mm_t6;                 \
        typedef __mm_t6 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        ct_typecheck_widenable_to((__mm_t) 0, d);                              \
        ct_typecheck_widenable_to((__mm_t) 0, e);                              \
        ct_typecheck_widenable_to((__mm_t) 0, f);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 < __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 < __mm_r ? __mm_c3 : __mm_r;                          \
        __mm_t __mm_c4 = (__mm_t) __mm_4;                                      \
        __mm_r = __mm_c4 < __mm_r ? __mm_c4 : __mm_r;                          \
        __mm_t __mm_c5 = (__mm_t) __mm_5;                                      \
        __mm_r = __mm_c5 < __mm_r ? __mm_c5 : __mm_r;                          \
        __mm_t __mm_c6 = (__mm_t) __mm_6;                                      \
        __mm_r = __mm_c6 < __mm_r ? __mm_c6 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MIN_7(a, b, c, d, e, f, g)                                            \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        __auto_type __mm_4 = (d);                                              \
        __auto_type __mm_5 = (e);                                              \
        __auto_type __mm_6 = (f);                                              \
        __auto_type __mm_7 = (g);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef ct_common_type_2((__mm_t3) 0, __mm_4) __mm_t4;                 \
        typedef ct_common_type_2((__mm_t4) 0, __mm_5) __mm_t5;                 \
        typedef ct_common_type_2((__mm_t5) 0, __mm_6) __mm_t6;                 \
        typedef ct_common_type_2((__mm_t6) 0, __mm_7) __mm_t7;                 \
        typedef __mm_t7 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        ct_typecheck_widenable_to((__mm_t) 0, d);                              \
        ct_typecheck_widenable_to((__mm_t) 0, e);                              \
        ct_typecheck_widenable_to((__mm_t) 0, f);                              \
        ct_typecheck_widenable_to((__mm_t) 0, g);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 < __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 < __mm_r ? __mm_c3 : __mm_r;                          \
        __mm_t __mm_c4 = (__mm_t) __mm_4;                                      \
        __mm_r = __mm_c4 < __mm_r ? __mm_c4 : __mm_r;                          \
        __mm_t __mm_c5 = (__mm_t) __mm_5;                                      \
        __mm_r = __mm_c5 < __mm_r ? __mm_c5 : __mm_r;                          \
        __mm_t __mm_c6 = (__mm_t) __mm_6;                                      \
        __mm_r = __mm_c6 < __mm_r ? __mm_c6 : __mm_r;                          \
        __mm_t __mm_c7 = (__mm_t) __mm_7;                                      \
        __mm_r = __mm_c7 < __mm_r ? __mm_c7 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MIN_8(a, b, c, d, e, f, g, h)                                         \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        __auto_type __mm_4 = (d);                                              \
        __auto_type __mm_5 = (e);                                              \
        __auto_type __mm_6 = (f);                                              \
        __auto_type __mm_7 = (g);                                              \
        __auto_type __mm_8 = (h);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef ct_common_type_2((__mm_t3) 0, __mm_4) __mm_t4;                 \
        typedef ct_common_type_2((__mm_t4) 0, __mm_5) __mm_t5;                 \
        typedef ct_common_type_2((__mm_t5) 0, __mm_6) __mm_t6;                 \
        typedef ct_common_type_2((__mm_t6) 0, __mm_7) __mm_t7;                 \
        typedef ct_common_type_2((__mm_t7) 0, __mm_8) __mm_t8;                 \
        typedef __mm_t8 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        ct_typecheck_widenable_to((__mm_t) 0, d);                              \
        ct_typecheck_widenable_to((__mm_t) 0, e);                              \
        ct_typecheck_widenable_to((__mm_t) 0, f);                              \
        ct_typecheck_widenable_to((__mm_t) 0, g);                              \
        ct_typecheck_widenable_to((__mm_t) 0, h);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 < __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 < __mm_r ? __mm_c3 : __mm_r;                          \
        __mm_t __mm_c4 = (__mm_t) __mm_4;                                      \
        __mm_r = __mm_c4 < __mm_r ? __mm_c4 : __mm_r;                          \
        __mm_t __mm_c5 = (__mm_t) __mm_5;                                      \
        __mm_r = __mm_c5 < __mm_r ? __mm_c5 : __mm_r;                          \
        __mm_t __mm_c6 = (__mm_t) __mm_6;                                      \
        __mm_r = __mm_c6 < __mm_r ? __mm_c6 : __mm_r;                          \
        __mm_t __mm_c7 = (__mm_t) __mm_7;                                      \
        __mm_r = __mm_c7 < __mm_r ? __mm_c7 : __mm_r;                          \
        __mm_t __mm_c8 = (__mm_t) __mm_8;                                      \
        __mm_r = __mm_c8 < __mm_r ? __mm_c8 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define MIN(...) PP_CALL(_MIN, __VA_ARGS__)

#define _MAX_1(a) (a)

#define _MAX_2(a, b)                                                           \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef __mm_t2 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 > __mm_r ? __mm_c2 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MAX_3(a, b, c)                                                        \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef __mm_t3 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 > __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 > __mm_r ? __mm_c3 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MAX_4(a, b, c, d)                                                     \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        __auto_type __mm_4 = (d);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef ct_common_type_2((__mm_t3) 0, __mm_4) __mm_t4;                 \
        typedef __mm_t4 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        ct_typecheck_widenable_to((__mm_t) 0, d);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 > __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 > __mm_r ? __mm_c3 : __mm_r;                          \
        __mm_t __mm_c4 = (__mm_t) __mm_4;                                      \
        __mm_r = __mm_c4 > __mm_r ? __mm_c4 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MAX_5(a, b, c, d, e)                                                  \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        __auto_type __mm_4 = (d);                                              \
        __auto_type __mm_5 = (e);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef ct_common_type_2((__mm_t3) 0, __mm_4) __mm_t4;                 \
        typedef ct_common_type_2((__mm_t4) 0, __mm_5) __mm_t5;                 \
        typedef __mm_t5 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        ct_typecheck_widenable_to((__mm_t) 0, d);                              \
        ct_typecheck_widenable_to((__mm_t) 0, e);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 > __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 > __mm_r ? __mm_c3 : __mm_r;                          \
        __mm_t __mm_c4 = (__mm_t) __mm_4;                                      \
        __mm_r = __mm_c4 > __mm_r ? __mm_c4 : __mm_r;                          \
        __mm_t __mm_c5 = (__mm_t) __mm_5;                                      \
        __mm_r = __mm_c5 > __mm_r ? __mm_c5 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MAX_6(a, b, c, d, e, f)                                               \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        __auto_type __mm_4 = (d);                                              \
        __auto_type __mm_5 = (e);                                              \
        __auto_type __mm_6 = (f);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef ct_common_type_2((__mm_t3) 0, __mm_4) __mm_t4;                 \
        typedef ct_common_type_2((__mm_t4) 0, __mm_5) __mm_t5;                 \
        typedef ct_common_type_2((__mm_t5) 0, __mm_6) __mm_t6;                 \
        typedef __mm_t6 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        ct_typecheck_widenable_to((__mm_t) 0, d);                              \
        ct_typecheck_widenable_to((__mm_t) 0, e);                              \
        ct_typecheck_widenable_to((__mm_t) 0, f);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 > __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 > __mm_r ? __mm_c3 : __mm_r;                          \
        __mm_t __mm_c4 = (__mm_t) __mm_4;                                      \
        __mm_r = __mm_c4 > __mm_r ? __mm_c4 : __mm_r;                          \
        __mm_t __mm_c5 = (__mm_t) __mm_5;                                      \
        __mm_r = __mm_c5 > __mm_r ? __mm_c5 : __mm_r;                          \
        __mm_t __mm_c6 = (__mm_t) __mm_6;                                      \
        __mm_r = __mm_c6 > __mm_r ? __mm_c6 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MAX_7(a, b, c, d, e, f, g)                                            \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        __auto_type __mm_4 = (d);                                              \
        __auto_type __mm_5 = (e);                                              \
        __auto_type __mm_6 = (f);                                              \
        __auto_type __mm_7 = (g);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef ct_common_type_2((__mm_t3) 0, __mm_4) __mm_t4;                 \
        typedef ct_common_type_2((__mm_t4) 0, __mm_5) __mm_t5;                 \
        typedef ct_common_type_2((__mm_t5) 0, __mm_6) __mm_t6;                 \
        typedef ct_common_type_2((__mm_t6) 0, __mm_7) __mm_t7;                 \
        typedef __mm_t7 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        ct_typecheck_widenable_to((__mm_t) 0, d);                              \
        ct_typecheck_widenable_to((__mm_t) 0, e);                              \
        ct_typecheck_widenable_to((__mm_t) 0, f);                              \
        ct_typecheck_widenable_to((__mm_t) 0, g);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 > __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 > __mm_r ? __mm_c3 : __mm_r;                          \
        __mm_t __mm_c4 = (__mm_t) __mm_4;                                      \
        __mm_r = __mm_c4 > __mm_r ? __mm_c4 : __mm_r;                          \
        __mm_t __mm_c5 = (__mm_t) __mm_5;                                      \
        __mm_r = __mm_c5 > __mm_r ? __mm_c5 : __mm_r;                          \
        __mm_t __mm_c6 = (__mm_t) __mm_6;                                      \
        __mm_r = __mm_c6 > __mm_r ? __mm_c6 : __mm_r;                          \
        __mm_t __mm_c7 = (__mm_t) __mm_7;                                      \
        __mm_r = __mm_c7 > __mm_r ? __mm_c7 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define _MAX_8(a, b, c, d, e, f, g, h)                                         \
    ({                                                                         \
        __auto_type __mm_1 = (a);                                              \
        __auto_type __mm_2 = (b);                                              \
        __auto_type __mm_3 = (c);                                              \
        __auto_type __mm_4 = (d);                                              \
        __auto_type __mm_5 = (e);                                              \
        __auto_type __mm_6 = (f);                                              \
        __auto_type __mm_7 = (g);                                              \
        __auto_type __mm_8 = (h);                                              \
        typedef ct_common_type_2(__mm_1, __mm_2) __mm_t2;                      \
        typedef ct_common_type_2((__mm_t2) 0, __mm_3) __mm_t3;                 \
        typedef ct_common_type_2((__mm_t3) 0, __mm_4) __mm_t4;                 \
        typedef ct_common_type_2((__mm_t4) 0, __mm_5) __mm_t5;                 \
        typedef ct_common_type_2((__mm_t5) 0, __mm_6) __mm_t6;                 \
        typedef ct_common_type_2((__mm_t6) 0, __mm_7) __mm_t7;                 \
        typedef ct_common_type_2((__mm_t7) 0, __mm_8) __mm_t8;                 \
        typedef __mm_t8 __mm_t;                                                \
        ct_typecheck_widenable_to((__mm_t) 0, a);                              \
        ct_typecheck_widenable_to((__mm_t) 0, b);                              \
        ct_typecheck_widenable_to((__mm_t) 0, c);                              \
        ct_typecheck_widenable_to((__mm_t) 0, d);                              \
        ct_typecheck_widenable_to((__mm_t) 0, e);                              \
        ct_typecheck_widenable_to((__mm_t) 0, f);                              \
        ct_typecheck_widenable_to((__mm_t) 0, g);                              \
        ct_typecheck_widenable_to((__mm_t) 0, h);                              \
        __mm_t __mm_r = (__mm_t) __mm_1;                                       \
        __mm_t __mm_c2 = (__mm_t) __mm_2;                                      \
        __mm_r = __mm_c2 > __mm_r ? __mm_c2 : __mm_r;                          \
        __mm_t __mm_c3 = (__mm_t) __mm_3;                                      \
        __mm_r = __mm_c3 > __mm_r ? __mm_c3 : __mm_r;                          \
        __mm_t __mm_c4 = (__mm_t) __mm_4;                                      \
        __mm_r = __mm_c4 > __mm_r ? __mm_c4 : __mm_r;                          \
        __mm_t __mm_c5 = (__mm_t) __mm_5;                                      \
        __mm_r = __mm_c5 > __mm_r ? __mm_c5 : __mm_r;                          \
        __mm_t __mm_c6 = (__mm_t) __mm_6;                                      \
        __mm_r = __mm_c6 > __mm_r ? __mm_c6 : __mm_r;                          \
        __mm_t __mm_c7 = (__mm_t) __mm_7;                                      \
        __mm_r = __mm_c7 > __mm_r ? __mm_c7 : __mm_r;                          \
        __mm_t __mm_c8 = (__mm_t) __mm_8;                                      \
        __mm_r = __mm_c8 > __mm_r ? __mm_c8 : __mm_r;                          \
        (__mm_t) __mm_r;                                                       \
    })

#define MAX(...) PP_CALL(_MAX, __VA_ARGS__)
