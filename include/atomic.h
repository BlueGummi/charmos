/* @title: Atomic Operations, Types & Primitives */
#pragma once

#include <compiler/atomic.h>
#include <compiler/core.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef _Atomic uint8_t atomic_uint8_t;
typedef _Atomic uint16_t atomic_uint16_t;
typedef _Atomic uint32_t atomic_uint32_t;
typedef _Atomic uint64_t atomic_uint64_t;
typedef _Atomic __uint128_t atomic_uint128_t;

typedef _Atomic int8_t atomic_int8_t;
typedef _Atomic int16_t atomic_int16_t;
typedef _Atomic int32_t atomic_int32_t;
typedef _Atomic int64_t atomic_int64_t;
typedef _Atomic __int128_t atomic_iint128_t;

typedef _Atomic bool atomic_bool;
typedef _Atomic size_t atomic_size_t;
typedef _Atomic uintptr_t atomic_uintptr_t;
typedef _Atomic intptr_t atomic_intptr_t;
typedef _Atomic ptrdiff_t atomic_ptrdiff_t;

/* Nice alias */
#define atomic(type) _Atomic(type)

/* ==== Memory Order ==== */
#define mo_rlx memory_order_relaxed
#define mo_consume memory_order_consume
#define mo_acquire memory_order_acquire
#define mo_release memory_order_release
#define mo_acq_rel memory_order_acq_rel
#define mo_seq_cst memory_order_seq_cst

/* ==== Memory Order Validations ==== */
#define atomic_check_not_consume_internal(mo)                                  \
    static_assert(!__builtin_constant_p(mo) || (mo) != memory_order_consume,   \
                  "mo_consume is prohibited, use mo_acquire or ca_read_once")

#define atomic_check_load_mo_internal(mo)                                      \
    static_assert(                                                             \
        !__builtin_constant_p(mo) ||                                           \
            ((mo) == memory_order_relaxed || (mo) == memory_order_acquire ||   \
             (mo) == memory_order_seq_cst),                                    \
        "Invalid memory order for atomic_load: cannot use release or acq_rel")

#define atomic_check_store_mo_internal(mo)                                     \
    static_assert(!__builtin_constant_p(mo) ||                                 \
                      ((mo) == memory_order_relaxed ||                         \
                       (mo) == memory_order_release ||                         \
                       (mo) == memory_order_seq_cst),                          \
                  "Invalid memory order for atomic_store: cannot use "         \
                  "acquire or acq_rel")

#define atomic_check_cas_fail_mo_internal(fail_mo)                             \
    static_assert(!__builtin_constant_p(fail_mo) ||                            \
                      ((fail_mo) != memory_order_release &&                    \
                       (fail_mo) != memory_order_acq_rel),                     \
                  "CAS failure memory order cannot be release or acq_rel")

#define atomic_overload_internal(name, ...)                                    \
    PP_DISPATCH(name, PP_NARG(__VA_ARGS__))
#define atomic_call_internal(name, ...)                                        \
    atomic_overload_internal(name, __VA_ARGS__)(__VA_ARGS__)

#undef atomic_load
#define atomic_load_checked_internal(ptr, mo)                                  \
    ({                                                                         \
        atomic_check_not_consume_internal(mo);                                 \
        atomic_check_load_mo_internal(mo);                                     \
        atomic_load_explicit((ptr), (mo));                                     \
    })
#define atomic_load_1(ptr) atomic_load_2((ptr), mo_seq_cst)
#define atomic_load_2(ptr, mo) atomic_load_checked_internal((ptr), (mo))
#define atomic_load(...) atomic_call_internal(atomic_load, __VA_ARGS__)

#undef atomic_store
#define atomic_store_checked_internal(ptr, val, mo)                            \
    ({                                                                         \
        atomic_check_not_consume_internal(mo);                                 \
        atomic_check_store_mo_internal(mo);                                    \
        atomic_store_explicit((ptr), (val), (mo));                             \
    })
#define atomic_store_2(ptr, val) atomic_store_3((ptr), (val), mo_seq_cst)
#define atomic_store_3(ptr, val, mo)                                           \
    atomic_store_checked_internal((ptr), (val), (mo))
#define atomic_store(...) atomic_call_internal(atomic_store, __VA_ARGS__)

#undef atomic_exchange
#define atomic_xchg_checked_internal(ptr, val, mo)                             \
    ({                                                                         \
        atomic_check_not_consume_internal(mo);                                 \
        atomic_exchange_explicit((ptr), (val), (mo));                          \
    })
#define atomic_exchange_2(ptr, val) atomic_exchange_3((ptr), (val), mo_seq_cst)
#define atomic_exchange_3(ptr, val, mo)                                        \
    atomic_xchg_checked_internal((ptr), (val), (mo))
#define atomic_exchange(...) atomic_call_internal(atomic_exchange, __VA_ARGS__)

#undef atomic_compare_exchange_weak
#define atomic_cas_weak_checked_internal(ptr, exp, des, succ, fail)            \
    ({                                                                         \
        atomic_check_not_consume_internal(succ);                               \
        atomic_check_not_consume_internal(fail);                               \
        atomic_check_cas_fail_mo_internal(fail);                               \
        atomic_compare_exchange_weak_explicit((ptr), (exp), (des), (succ),     \
                                              (fail));                         \
    })
#define atomic_compare_exchange_weak_3(ptr, exp, des)                          \
    atomic_compare_exchange_weak_5((ptr), (exp), (des), mo_seq_cst, mo_seq_cst)
#define atomic_compare_exchange_weak_4(ptr, exp, des, mo)                      \
    ({                                                                         \
        static_assert(0, "4-argument CAS is prohibited");                      \
        false;                                                                 \
    })
#define atomic_compare_exchange_weak_5(ptr, exp, des, succ, fail)              \
    atomic_cas_weak_checked_internal((ptr), (exp), (des), (succ), (fail))
#define atomic_compare_exchange_weak(...)                                      \
    atomic_call_internal(atomic_compare_exchange_weak, __VA_ARGS__)
#define atomic_cas_weak(...) atomic_compare_exchange_weak(__VA_ARGS__)

#undef atomic_compare_exchange_strong
#define atomic_cas_strong_checked_internal(ptr, exp, des, succ, fail)          \
    ({                                                                         \
        atomic_check_not_consume_internal(succ);                               \
        atomic_check_not_consume_internal(fail);                               \
        atomic_check_cas_fail_mo_internal(fail);                               \
        atomic_compare_exchange_strong_explicit((ptr), (exp), (des), (succ),   \
                                                (fail));                       \
    })
#define atomic_compare_exchange_strong_3(ptr, exp, des)                        \
    atomic_compare_exchange_strong_5((ptr), (exp), (des), mo_seq_cst,          \
                                     mo_seq_cst)
#define atomic_compare_exchange_strong_4(ptr, exp, des, mo)                    \
    ({                                                                         \
        static_assert(0, "4-argument CAS is prohibited");                      \
        false;                                                                 \
    })
#define atomic_compare_exchange_strong_5(ptr, exp, des, succ, fail)            \
    atomic_cas_strong_checked_internal((ptr), (exp), (des), (succ), (fail))
#define atomic_compare_exchange_strong(...)                                    \
    atomic_call_internal(atomic_compare_exchange_strong, __VA_ARGS__)
#define atomic_cas_strong(...) atomic_compare_exchange_strong(__VA_ARGS__)

/* Linux-style value-returning atomic_cmpxchg */
#define atomic_cmpxchg_checked_internal(ptr, old_val, new_val, succ, fail)     \
    ({                                                                         \
        __auto_type cpx_ptr_internal = (ptr);                                  \
        typeof((void) 0, *cpx_ptr_internal) cpx_exp_internal = (old_val);      \
        (void) atomic_compare_exchange_strong_explicit(                        \
            cpx_ptr_internal, &cpx_exp_internal, (new_val), (succ), (fail));   \
        cpx_exp_internal;                                                      \
    })
#define atomic_cmpxchg_3(ptr, old_val, new_val)                                \
    atomic_cmpxchg_5((ptr), (old_val), (new_val), mo_seq_cst, mo_seq_cst)
#define atomic_cmpxchg_4(ptr, old_val, new_val, mo)                            \
    ({                                                                         \
        static_assert(0, "4-argument cmpxchg is prohibited");                  \
        *(ptr);                                                                \
    })
#define atomic_cmpxchg_5(ptr, old_val, new_val, succ, fail)                    \
    atomic_cmpxchg_checked_internal((ptr), (old_val), (new_val), (succ), (fail))
#define atomic_cmpxchg(...) atomic_call_internal(atomic_cmpxchg, __VA_ARGS__)

/* Arithmetic and Bitwise */
#undef atomic_fetch_add
#define atomic_fetch_add_checked_internal(ptr, val, mo)                        \
    ({                                                                         \
        atomic_check_not_consume_internal(mo);                                 \
        atomic_fetch_add_explicit((ptr), (val), (mo));                         \
    })
#define atomic_fetch_add_2(ptr, val)                                           \
    atomic_fetch_add_3((ptr), (val), mo_seq_cst)
#define atomic_fetch_add_3(ptr, val, mo)                                       \
    atomic_fetch_add_checked_internal((ptr), (val), (mo))
#define atomic_fetch_add(...)                                                  \
    atomic_call_internal(atomic_fetch_add, __VA_ARGS__)

#undef atomic_fetch_sub
#define atomic_fetch_sub_checked_internal(ptr, val, mo)                        \
    ({                                                                         \
        atomic_check_not_consume_internal(mo);                                 \
        atomic_fetch_sub_explicit((ptr), (val), (mo));                         \
    })
#define atomic_fetch_sub_2(ptr, val)                                           \
    atomic_fetch_sub_3((ptr), (val), mo_seq_cst)
#define atomic_fetch_sub_3(ptr, val, mo)                                       \
    atomic_fetch_sub_checked_internal((ptr), (val), (mo))
#define atomic_fetch_sub(...)                                                  \
    atomic_call_internal(atomic_fetch_sub, __VA_ARGS__)

#undef atomic_fetch_and
#define atomic_fetch_and_checked_internal(ptr, val, mo)                        \
    ({                                                                         \
        atomic_check_not_consume_internal(mo);                                 \
        atomic_fetch_and_explicit((ptr), (val), (mo));                         \
    })
#define atomic_fetch_and_2(ptr, val)                                           \
    atomic_fetch_and_3((ptr), (val), mo_seq_cst)
#define atomic_fetch_and_3(ptr, val, mo)                                       \
    atomic_fetch_and_checked_internal((ptr), (val), (mo))
#define atomic_fetch_and(...)                                                  \
    atomic_call_internal(atomic_fetch_and, __VA_ARGS__)

#undef atomic_fetch_or
#define atomic_fetch_or_checked_internal(ptr, val, mo)                         \
    ({                                                                         \
        atomic_check_not_consume_internal(mo);                                 \
        atomic_fetch_or_explicit((ptr), (val), (mo));                          \
    })
#define atomic_fetch_or_2(ptr, val) atomic_fetch_or_3((ptr), (val), mo_seq_cst)
#define atomic_fetch_or_3(ptr, val, mo)                                        \
    atomic_fetch_or_checked_internal((ptr), (val), (mo))
#define atomic_fetch_or(...) atomic_call_internal(atomic_fetch_or, __VA_ARGS__)

#undef atomic_fetch_xor
#define atomic_fetch_xor_checked_internal(ptr, val, mo)                        \
    ({                                                                         \
        atomic_check_not_consume_internal(mo);                                 \
        atomic_fetch_xor_explicit((ptr), (val), (mo));                         \
    })
#define atomic_fetch_xor_2(ptr, val)                                           \
    atomic_fetch_xor_3((ptr), (val), mo_seq_cst)
#define atomic_fetch_xor_3(ptr, val, mo)                                       \
    atomic_fetch_xor_checked_internal((ptr), (val), (mo))
#define atomic_fetch_xor(...)                                                  \
    atomic_call_internal(atomic_fetch_xor, __VA_ARGS__)

/* Atomic Flags */
#undef atomic_flag_test_and_set
#define atomic_flag_test_and_set_checked_internal(ptr, mo)                     \
    ({                                                                         \
        atomic_check_not_consume_internal(mo);                                 \
        atomic_flag_test_and_set_explicit((ptr), (mo));                        \
    })
#define atomic_flag_test_and_set_1(ptr)                                        \
    atomic_flag_test_and_set_2((ptr), mo_seq_cst)
#define atomic_flag_test_and_set_2(ptr, mo)                                    \
    atomic_flag_test_and_set_checked_internal((ptr), (mo))
#define atomic_flag_test_and_set(...)                                          \
    atomic_call_internal(atomic_flag_test_and_set, __VA_ARGS__)

#undef atomic_flag_clear
#define atomic_flag_clear_checked_internal(ptr, mo)                            \
    ({                                                                         \
        atomic_check_not_consume_internal(mo);                                 \
        atomic_check_store_mo_internal(mo);                                    \
        atomic_flag_clear_explicit((ptr), (mo));                               \
    })
#define atomic_flag_clear_1(ptr) atomic_flag_clear_2((ptr), mo_seq_cst)
#define atomic_flag_clear_2(ptr, mo)                                           \
    atomic_flag_clear_checked_internal((ptr), (mo))
#define atomic_flag_clear(...)                                                 \
    atomic_call_internal(atomic_flag_clear, __VA_ARGS__)

/* ==== Mnemonic Suffixes ==== */
#define atomic_load_rlx(ptr) atomic_load((ptr), mo_rlx)
#define atomic_load_acq(ptr) atomic_load((ptr), mo_acquire)

#define atomic_store_rlx(ptr, val) atomic_store((ptr), (val), mo_rlx)
#define atomic_store_release(ptr, val) atomic_store((ptr), (val), mo_release)

#define atomic_xchg_rlx(ptr, val) atomic_exchange((ptr), (val), mo_rlx)
#define atomic_xchg_acq(ptr, val) atomic_exchange((ptr), (val), mo_acquire)
#define atomic_xchg_release(ptr, val) atomic_exchange((ptr), (val), mo_release)
#define atomic_xchg_acq_rel(ptr, val) atomic_exchange((ptr), (val), mo_acq_rel)

/* Fetch arithmetic and bitwise shorthands */
#define atomic_fetch_add_rlx(ptr, val) atomic_fetch_add((ptr), (val), mo_rlx)
#define atomic_fetch_add_acq(ptr, val)                                         \
    atomic_fetch_add((ptr), (val), mo_acquire)
#define atomic_fetch_add_release(ptr, val)                                     \
    atomic_fetch_add((ptr), (val), mo_release)
#define atomic_fetch_add_acq_rel(ptr, val)                                     \
    atomic_fetch_add((ptr), (val), mo_acq_rel)

#define atomic_fetch_sub_rlx(ptr, val) atomic_fetch_sub((ptr), (val), mo_rlx)
#define atomic_fetch_sub_acq(ptr, val)                                         \
    atomic_fetch_sub((ptr), (val), mo_acquire)
#define atomic_fetch_sub_release(ptr, val)                                     \
    atomic_fetch_sub((ptr), (val), mo_release)
#define atomic_fetch_sub_acq_rel(ptr, val)                                     \
    atomic_fetch_sub((ptr), (val), mo_acq_rel)

#define atomic_fetch_or_rlx(ptr, val) atomic_fetch_or((ptr), (val), mo_rlx)
#define atomic_fetch_or_acq(ptr, val) atomic_fetch_or((ptr), (val), mo_acquire)
#define atomic_fetch_or_release(ptr, val)                                      \
    atomic_fetch_or((ptr), (val), mo_release)
#define atomic_fetch_or_acq_rel(ptr, val)                                      \
    atomic_fetch_or((ptr), (val), mo_acq_rel)

#define atomic_fetch_and_rlx(ptr, val) atomic_fetch_and((ptr), (val), mo_rlx)
#define atomic_fetch_and_acq(ptr, val)                                         \
    atomic_fetch_and((ptr), (val), mo_acquire)
#define atomic_fetch_and_release(ptr, val)                                     \
    atomic_fetch_and((ptr), (val), mo_release)
#define atomic_fetch_and_acq_rel(ptr, val)                                     \
    atomic_fetch_and((ptr), (val), mo_acq_rel)

#define atomic_fetch_xor_rlx(ptr, val) atomic_fetch_xor((ptr), (val), mo_rlx)
#define atomic_fetch_xor_acq(ptr, val)                                         \
    atomic_fetch_xor((ptr), (val), mo_acquire)
#define atomic_fetch_xor_release(ptr, val)                                     \
    atomic_fetch_xor((ptr), (val), mo_release)
#define atomic_fetch_xor_acq_rel(ptr, val)                                     \
    atomic_fetch_xor((ptr), (val), mo_acq_rel)

/* Flag shorthands */
#define atomic_flag_test_and_set_rlx(ptr)                                      \
    atomic_flag_test_and_set((ptr), mo_rlx)
#define atomic_flag_test_and_set_acq(ptr)                                      \
    atomic_flag_test_and_set((ptr), mo_acquire)
#define atomic_flag_test_and_set_acq_rel(ptr)                                  \
    atomic_flag_test_and_set((ptr), mo_acq_rel)
#define atomic_flag_clear_rlx(ptr) atomic_flag_clear((ptr), mo_rlx)
#define atomic_flag_clear_release(ptr) atomic_flag_clear((ptr), mo_release)

/* ==== Arithmetic Shorthands ==== */
#define atomic_inc_rlx(ptr) atomic_inc((ptr), mo_rlx)
#define atomic_inc_acq(ptr) atomic_inc((ptr), mo_acquire)
#define atomic_inc_release(ptr) atomic_inc((ptr), mo_release)
#define atomic_inc_acq_rel(ptr) atomic_inc((ptr), mo_acq_rel)

#define atomic_dec_rlx(ptr) atomic_dec((ptr), mo_rlx)
#define atomic_dec_acq(ptr) atomic_dec((ptr), mo_acquire)
#define atomic_dec_release(ptr) atomic_dec((ptr), mo_release)
#define atomic_dec_acq_rel(ptr) atomic_dec((ptr), mo_acq_rel)
#define atomic_inc_1(ptr) atomic_inc_2((ptr), mo_seq_cst)
#define atomic_inc_2(ptr, mo) atomic_fetch_add_checked_internal((ptr), 1, (mo))
#define atomic_inc(...) atomic_call_internal(atomic_inc, __VA_ARGS__)

#define atomic_dec_1(ptr) atomic_dec_2((ptr), mo_seq_cst)
#define atomic_dec_2(ptr, mo) atomic_fetch_sub_checked_internal((ptr), 1, (mo))
#define atomic_dec(...) atomic_call_internal(atomic_dec, __VA_ARGS__)

#define atomic_inc_return_1(ptr) atomic_inc_return_2((ptr), mo_seq_cst)
#define atomic_inc_return_2(ptr, mo)                                           \
    (atomic_fetch_add_checked_internal((ptr), 1, (mo)) + 1)
#define atomic_inc_return(...)                                                 \
    atomic_call_internal(atomic_inc_return, __VA_ARGS__)

#define atomic_dec_return_1(ptr) atomic_dec_return_2((ptr), mo_seq_cst)
#define atomic_dec_return_2(ptr, mo)                                           \
    (atomic_fetch_sub_checked_internal((ptr), 1, (mo)) - 1)
#define atomic_dec_return(...)                                                 \
    atomic_call_internal(atomic_dec_return, __VA_ARGS__)

#define atomic_inc_return_rlx(ptr) atomic_inc_return((ptr), mo_rlx)
#define atomic_inc_return_acq(ptr) atomic_inc_return((ptr), mo_acquire)
#define atomic_inc_return_release(ptr) atomic_inc_return((ptr), mo_release)
#define atomic_inc_return_acq_rel(ptr) atomic_inc_return((ptr), mo_acq_rel)

#define atomic_dec_return_rlx(ptr) atomic_dec_return((ptr), mo_rlx)
#define atomic_dec_return_acq(ptr) atomic_dec_return((ptr), mo_acquire)
#define atomic_dec_return_release(ptr) atomic_dec_return((ptr), mo_release)
#define atomic_dec_return_acq_rel(ptr) atomic_dec_return((ptr), mo_acq_rel)

#define atomic_dec_and_test(ptr)                                               \
    (atomic_fetch_sub_checked_internal((ptr), 1, mo_acq_rel) == 1)

/* ==== Extremum & Watermark Operations ==== */
#define atomic_fetch_max_explicit(ptr, val, succ_mo, fail_mo)                  \
    ({                                                                         \
        __auto_type fm_ptr_internal = (ptr);                                   \
        typeof((void) 0, *fm_ptr_internal) fm_val_internal = (val);            \
        typeof((void) 0, *fm_ptr_internal) fm_old_internal =                   \
            atomic_load_checked_internal(fm_ptr_internal, (fail_mo));          \
        while (fm_val_internal > fm_old_internal &&                            \
               !atomic_cas_weak_checked_internal(                              \
                   fm_ptr_internal, &fm_old_internal, fm_val_internal,         \
                   (succ_mo), (fail_mo)))                                      \
            ;                                                                  \
        fm_old_internal;                                                       \
    })

#define atomic_fetch_max_2(ptr, val)                                           \
    atomic_fetch_max_4((ptr), (val), mo_seq_cst, mo_seq_cst)
#define atomic_fetch_max_3(ptr, val, mo)                                       \
    ({                                                                         \
        static_assert(0, "3-argument atomic_fetch_max is prohibited");         \
        *(ptr);                                                                \
    })
#define atomic_fetch_max_4(ptr, val, succ_mo, fail_mo)                         \
    atomic_fetch_max_explicit((ptr), (val), (succ_mo), (fail_mo))
#define atomic_fetch_max(...)                                                  \
    atomic_call_internal(atomic_fetch_max, __VA_ARGS__)

#define atomic_fetch_min_explicit(ptr, val, succ_mo, fail_mo)                  \
    ({                                                                         \
        __auto_type fn_ptr_internal = (ptr);                                   \
        typeof((void) 0, *fn_ptr_internal) fn_val_internal = (val);            \
        typeof((void) 0, *fn_ptr_internal) fn_old_internal =                   \
            atomic_load_checked_internal(fn_ptr_internal, (fail_mo));          \
        while (fn_val_internal < fn_old_internal &&                            \
               !atomic_cas_weak_checked_internal(                              \
                   fn_ptr_internal, &fn_old_internal, fn_val_internal,         \
                   (succ_mo), (fail_mo)))                                      \
            ;                                                                  \
        fn_old_internal;                                                       \
    })

#define atomic_fetch_min_2(ptr, val)                                           \
    atomic_fetch_min_4((ptr), (val), mo_seq_cst, mo_seq_cst)
#define atomic_fetch_min_3(ptr, val, mo)                                       \
    ({                                                                         \
        static_assert(0, "3-argument atomic_fetch_min is prohibited");         \
        *(ptr);                                                                \
    })
#define atomic_fetch_min_4(ptr, val, succ_mo, fail_mo)                         \
    atomic_fetch_min_explicit((ptr), (val), (succ_mo), (fail_mo))
#define atomic_fetch_min(...)                                                  \
    atomic_call_internal(atomic_fetch_min, __VA_ARGS__)

/* ==== RMW CAS Loop Helper ==== */
#define atomic_update_explicit(ptr, old_var, new_expr, succ_mo, fail_mo)       \
    ({                                                                         \
        __auto_type u_ptr_internal = (ptr);                                    \
        typeof((void) 0, *u_ptr_internal) old_var =                            \
            atomic_load_checked_internal(u_ptr_internal, (fail_mo));           \
        while (!atomic_cas_weak_checked_internal(                              \
            u_ptr_internal, &(old_var), (new_expr), (succ_mo), (fail_mo)))     \
            ;                                                                  \
        old_var;                                                               \
    })

#define atomic_update(ptr, old_var, new_expr)                                  \
    atomic_update_explicit((ptr), old_var, (new_expr), mo_seq_cst, mo_seq_cst)

/* ==== Bitwise Operations ==== */
#define atomic_set_bit_explicit(ptr, bit, mo)                                  \
    atomic_fetch_or_checked_internal(                                          \
        (ptr), ((typeof((void) 0, *(ptr))) 1) << (bit), (mo))

#define atomic_clear_bit_explicit(ptr, bit, mo)                                \
    atomic_fetch_and_checked_internal(                                         \
        (ptr), ~(((typeof((void) 0, *(ptr))) 1) << (bit)), (mo))

#define atomic_toggle_bit_explicit(ptr, bit, mo)                               \
    atomic_fetch_xor_checked_internal(                                         \
        (ptr), ((typeof((void) 0, *(ptr))) 1) << (bit), (mo))

#define atomic_test_bit_explicit(ptr, bit, mo)                                 \
    ((atomic_load_checked_internal((ptr), (mo)) &                              \
      (((typeof((void) 0, *(ptr))) 1) << (bit))) != 0)

#define atomic_test_and_set_bit_explicit(ptr, bit, mo)                         \
    ((atomic_fetch_or_checked_internal(                                        \
          (ptr), ((typeof((void) 0, *(ptr))) 1) << (bit), (mo)) &              \
      (((typeof((void) 0, *(ptr))) 1) << (bit))) != 0)

#define atomic_test_and_clear_bit_explicit(ptr, bit, mo)                       \
    ((atomic_fetch_and_checked_internal(                                       \
          (ptr), ~(((typeof((void) 0, *(ptr))) 1) << (bit)), (mo)) &           \
      (((typeof((void) 0, *(ptr))) 1) << (bit))) != 0)

#define atomic_set_bit_2(ptr, bit) atomic_set_bit_3((ptr), (bit), mo_seq_cst)
#define atomic_set_bit_3(ptr, bit, mo)                                         \
    atomic_set_bit_explicit((ptr), (bit), (mo))
#define atomic_set_bit(...) atomic_call_internal(atomic_set_bit, __VA_ARGS__)

#define atomic_clear_bit_2(ptr, bit)                                           \
    atomic_clear_bit_3((ptr), (bit), mo_seq_cst)
#define atomic_clear_bit_3(ptr, bit, mo)                                       \
    atomic_clear_bit_explicit((ptr), (bit), (mo))
#define atomic_clear_bit(...)                                                  \
    atomic_call_internal(atomic_clear_bit, __VA_ARGS__)

#define atomic_toggle_bit_2(ptr, bit)                                          \
    atomic_toggle_bit_3((ptr), (bit), mo_seq_cst)
#define atomic_toggle_bit_3(ptr, bit, mo)                                      \
    atomic_toggle_bit_explicit((ptr), (bit), (mo))
#define atomic_toggle_bit(...)                                                 \
    atomic_call_internal(atomic_toggle_bit, __VA_ARGS__)

#define atomic_test_bit_2(ptr, bit) atomic_test_bit_3((ptr), (bit), mo_seq_cst)
#define atomic_test_bit_3(ptr, bit, mo)                                        \
    atomic_test_bit_explicit((ptr), (bit), (mo))
#define atomic_test_bit(...) atomic_call_internal(atomic_test_bit, __VA_ARGS__)

#define atomic_test_and_set_bit_2(ptr, bit)                                    \
    atomic_test_and_set_bit_3((ptr), (bit), mo_seq_cst)
#define atomic_test_and_set_bit_3(ptr, bit, mo)                                \
    atomic_test_and_set_bit_explicit((ptr), (bit), (mo))
#define atomic_test_and_set_bit(...)                                           \
    atomic_call_internal(atomic_test_and_set_bit, __VA_ARGS__)

#define atomic_test_and_clear_bit_2(ptr, bit)                                  \
    atomic_test_and_clear_bit_3((ptr), (bit), mo_seq_cst)
#define atomic_test_and_clear_bit_3(ptr, bit, mo)                              \
    atomic_test_and_clear_bit_explicit((ptr), (bit), (mo))
#define atomic_test_and_clear_bit(...)                                         \
    atomic_call_internal(atomic_test_and_clear_bit, __VA_ARGS__)

#define atomic_set_bit_rlx(ptr, bit) atomic_set_bit((ptr), (bit), mo_rlx)
#define atomic_set_bit_release(ptr, bit)                                       \
    atomic_set_bit((ptr), (bit), mo_release)

#define atomic_clear_bit_rlx(ptr, bit) atomic_clear_bit((ptr), (bit), mo_rlx)
#define atomic_clear_bit_release(ptr, bit)                                     \
    atomic_clear_bit((ptr), (bit), mo_release)

#define atomic_toggle_bit_rlx(ptr, bit) atomic_toggle_bit((ptr), (bit), mo_rlx)
#define atomic_test_bit_rlx(ptr, bit) atomic_test_bit((ptr), (bit), mo_rlx)

#define atomic_test_bit_acq(ptr, bit) atomic_test_bit((ptr), (bit), mo_acquire)

#define atomic_test_and_set_bit_acq(ptr, bit)                                  \
    atomic_test_and_set_bit((ptr), (bit), mo_acquire)
#define atomic_test_and_set_bit_acq_rel(ptr, bit)                              \
    atomic_test_and_set_bit((ptr), (bit), mo_acq_rel)
#define atomic_test_and_clear_bit_release(ptr, bit)                            \
    atomic_test_and_clear_bit((ptr), (bit), mo_release)
#define atomic_test_and_clear_bit_acq_rel(ptr, bit)                            \
    atomic_test_and_clear_bit((ptr), (bit), mo_acq_rel)

/* ==== Polling Primitives ==== */

#define atomic_poll_until(condition_expr)                                      \
    do {                                                                       \
        while (!(condition_expr)) {                                            \
            ca_pause();                                                        \
        }                                                                      \
    } while (0)

#define atomic_poll_until_eq_3(ptr, val, mo)                                   \
    do {                                                                       \
        __auto_type p_ptr_internal = (ptr);                                    \
        typeof((void) 0, *p_ptr_internal) p_val_internal = (val);              \
        while (atomic_load_checked_internal(p_ptr_internal, (mo)) !=           \
               p_val_internal) {                                               \
            ca_pause();                                                        \
        }                                                                      \
    } while (0)

#define atomic_poll_until_eq_2(ptr, val)                                       \
    atomic_poll_until_eq_3((ptr), (val), mo_seq_cst)

#define atomic_poll_until_eq(...)                                              \
    atomic_call_internal(atomic_poll_until_eq, __VA_ARGS__)

/* ==== Cacheline Alignment ==== */

#ifndef ca_cache_aligned
#define ca_cache_aligned cc_cache_aligned
#endif

#define CA_CACHELINE_SIZE 64

#define ca_cacheline_member(T, name)                                           \
    struct {                                                                   \
        uint8_t ca_pre_##name[CA_CACHELINE_SIZE - _Alignof(T)];                \
        union {                                                                \
            T name;                                                            \
            uint8_t ca_post_##name[CA_CACHELINE_SIZE];                         \
        };                                                                     \
    }

#ifndef TSA_LOCKLESS
#define TSA_LOCKLESS
#endif

/* ==== Lock-Free Assertions ==== */

#define ct_assert_atomic_lock_free(type)                                       \
    static_assert(__atomic_always_lock_free(sizeof(type), 0),                  \
                  #type " must be natively lock-free on this architecture")

ct_assert_atomic_lock_free(uint8_t);
ct_assert_atomic_lock_free(uint16_t);
ct_assert_atomic_lock_free(uint32_t);
ct_assert_atomic_lock_free(uint64_t);
ct_assert_atomic_lock_free(void *);
