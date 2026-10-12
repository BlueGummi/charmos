/* @title: Type aliases */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Time units */
ct_strong_int(time_ns, TIME_NS, uint64_t, UINT64_MAX);
ct_strong_int(time_us, TIME_US, uint64_t, UINT64_MAX);
ct_strong_int(time_ms, TIME_MS, uint64_t, UINT64_MAX);
ct_strong_int(time_s, TIME_S, uint64_t, UINT64_MAX);
typedef uint64_t timestamp_t;

/* Frequency */
ct_strong_int(freq_hz, FREQ_HZ, uint64_t, UINT64_MAX);
ct_strong_int(freq_khz, FREQ_KHZ, uint64_t, UINT64_MAX);
ct_strong_int(freq_mhz, FREQ_MHZ, uint64_t, UINT64_MAX);
ct_strong_int(freq_ghz, FREQ_GHZ, uint64_t, UINT64_MAX);

/* Size (binary/IEC units, powers of 1024) */
ct_strong_int(sz_kib, SZ_KIB, size_t, SIZE_MAX);
ct_strong_int(sz_mib, SZ_MIB, size_t, SIZE_MAX);
ct_strong_int(sz_gib, SZ_GIB, size_t, SIZE_MAX);
ct_strong_int(sz_tib, SZ_TIB, size_t, SIZE_MAX);

/* Size (decimal/SI units, powers of 1000) */
ct_strong_int(sz_kb, SZ_KB, size_t, SIZE_MAX);
ct_strong_int(sz_mb, SZ_MB, size_t, SIZE_MAX);
ct_strong_int(sz_gb, SZ_GB, size_t, SIZE_MAX);
ct_strong_int(sz_tb, SZ_TB, size_t, SIZE_MAX);

/* Bytes */
ct_strong_int(sz_b, SZ_B, size_t, SIZE_MAX);

/* Aliases over UNIX identifiers */
ct_strong_int(inode, INODE, uint32_t, UINT32_MAX);
typedef uint16_t mode_t;
typedef uint32_t gid_t;
typedef uint32_t uid_t;

/* For stack depot: someday will become uint32_t */
typedef struct stack_depot_record *stack_handle_t;

/* Refcount */
typedef atomic_uint32_t refcount_t;
typedef atomic_uint32_t mapcount_t;

/* Addresses and memory */
ct_strong_int(paddr, PADDR, uintptr_t, UINTPTR_MAX);
ct_strong_int(vaddr, VADDR, uintptr_t, UINTPTR_MAX);
ct_strong_int(pfn, PFN, uintptr_t, UINTPTR_MAX);
ct_strong_int(pgoff, PGOFF, uintptr_t, UINTPTR_MAX);
ct_strong_int(iova, IOVA, uintptr_t, UINTPTR_MAX);
ct_strong_int(pte, PTE, uint64_t, UINT64_MAX);
typedef uint64_t page_flags_t;

/* Processor topology and threading */
typedef uint8_t irq_t;
typedef int8_t cpu_perf_t;
ct_strong_int(cpu_id, CPU_ID, size_t, SIZE_MAX);
ct_strong_int(domain_id, DOMAIN_ID, size_t, SIZE_MAX);
ct_strong_int(numa_node, NUMA_NODE, size_t, SIZE_MAX);
ct_strong_int(thread_id, THREAD_ID, size_t, SIZE_MAX);
typedef int32_t nice_t;
typedef uint32_t thread_prio_t;

/* Other types */
typedef int64_t fx32_32_t;
typedef ptrdiff_t ssize_t;

/* Larger types */
typedef __int128_t int128_t;
typedef __uint128_t uint128_t;

/* "generic" function pointer, our low budget "We have _Any_func at home"
 * while C2y deliberates. Probably the only place where we typedef
 * a struct for a piece of the kernel proper, and using an address_space
 * on Clang */
struct any_fn_uncallable;
typedef struct {
#if defined(cn_id_clang)
    void cc_mem_any_fn *erased;
#else
    void (*erased)(struct any_fn_uncallable);
#endif
} any_fn_t;
ct_assert_same_size(any_fn_t, uintptr_t);

#define any_fn_erase(addr)                                                     \
    ((any_fn_t) {.erased = (typeof(((any_fn_t *) 0)->erased)) (addr)})

/* Constant expression for a function in static init */
#define any_fn_from(fn)                                                        \
    any_fn_erase(                                                              \
        (uintptr_t) (fn) +                                                     \
        ct_expr_assert_zero(ct_is_fn_ptr(fn), "`" #fn "` is not a function"))

#define any_fn_to(any, T)                                                      \
    (ct_typecheck(any_fn_t, any),                                              \
     (T) ((uintptr_t) (any).erased +                                           \
          ct_expr_assert_zero(ct_is_fn_ptr((T) 0),                             \
                              "`" #T "` is not a function pointer type")))

#define any_fn_eq(a, b)                                                        \
    (ct_typecheck(any_fn_t, a), ct_typecheck(any_fn_t, b),                     \
     (a).erased == (b).erased)

#define any_fn_is_null(any) (ct_typecheck(any_fn_t, any), (any).erased == NULL)

/* Sneak it through a void * data slot, which is ok on our targets */
#define any_fn_to_data(any)                                                    \
    (ct_typecheck(any_fn_t, any), (void *) (uintptr_t) (any).erased)

#define any_fn_from_data(ptr)                                                  \
    (ct_typecheck(void *, ptr), any_fn_erase((uintptr_t) (ptr)))

#define CPU_PERF_MAX INT8_MAX
#define CPU_PERF_MIN INT8_MIN

#define MODE_MAX UINT16_MAX
#define MODE_MIN 0

#define GID_MAX UINT32_MAX
#define GID_MIN 0

#define UID_MAX UINT32_MAX
#define UID_MIN 0

#define STACK_HANDLE_MAX UINT32_MAX
#define STACK_HANDLE_MIN 0

#define REFCOUNT_MAX UINT32_MAX
#define REFCOUNT_MIN 0

#define CPU_ID_NONE CPU_ID_MAX

#define DOMAIN_ID_NONE DOMAIN_ID_MAX

#define THREAD_ID_NONE THREAD_ID_MAX

#define PAGE_FLAGS_MAX UINT64_MAX
#define PAGE_FLAGS_MIN 0

#define THREAD_PRIO_MAX UINT32_MAX
#define THREAD_PRIO_MIN 0

#define FX32_32_MAX INT64_MAX
#define FX32_32_MIN INT64_MIN

#define NICE_MAX 19
#define NICE_MIN -20

#define SSIZE_MAX ((ssize_t) (SIZE_MAX >> 1))
#define SSIZE_MIN ((ssize_t) (-SSIZE_MAX - 1))

/* Shift has to come after the complement, because complementing the
 * shifted zero yields all ones, turning it into `-1` */
#define INT128_MAX ((int128_t) (~((uint128_t) 0ULL) >> 1))
#define INT128_MIN ((int128_t) (-INT128_MAX - 1))

#define UINT128_MAX ((uint128_t) (~((uint128_t) 0ULL)))
#define UINT128_MIN ((uint128_t) 0ULL)
