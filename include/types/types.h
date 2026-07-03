/* @title: Type aliases */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t irq_t;
typedef int8_t cpu_perf_t;
typedef uint64_t time_t;
typedef uint64_t timestamp_t;
typedef uint32_t inode_t;
typedef uint16_t mode_t;
typedef uint32_t gid_t;
typedef uint32_t uid_t;
typedef _Atomic uint32_t refcount_t;
typedef _Atomic uint32_t mapcount_t;
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;
typedef uintptr_t pfn_t;
typedef uintptr_t pgoff_t;
typedef uintptr_t iova_t;
typedef size_t cpu_id_t;
typedef size_t numa_node_t;
typedef uint64_t pte_t;
typedef uint64_t page_flags_t;
typedef uint32_t thread_prio_t;
typedef int64_t fx32_32_t;
typedef int32_t nice_t;
typedef ptrdiff_t ssize_t;
typedef __int128_t int128_t;
typedef __uint128_t uint128_t;

#define CPU_PERF_MAX INT8_MAX
#define CPU_PERF_MIN INT8_MIN

#define TIME_MAX UINT64_MAX
#define TIME_MIN 0

#define INODE_MAX UINT32_MAX
#define INODE_MIN 0

#define MODE_MAX UINT16_MAX
#define MODE_MIN 0

#define GID_MAX UINT32_MAX
#define GID_MIN 0

#define UID_MAX UINT32_MAX
#define UID_MIN 0

#define REFCOUNT_MAX UINT32_MAX
#define REFCOUNT_MIN 0

#define PADDR_MAX UINTPTR_MAX
#define PADDR_MIN 0

#define VADDR_MAX UINTPTR_MAX
#define VADDR_MIN 0

#define IOVA_MAX UINTPTR_MAX
#define IOVA_MIN 0

#define CPU_ID_MAX SIZE_MAX
#define CPU_ID_MIN 0

#define NUMA_NODE_MAX SIZE_MAX
#define NUMA_NODE_MIN 0

#define PTE_MAX UINT64_MAX
#define PTE_MIN 0

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

#define INT128_MAX ((int128_t) (int128_t) (~((uint128_t) 0ULL >> 1)))
#define INT128_MIN ((int128_t) (-INT128_MAX - 1))
#define UINT128_MAX ((uint128_t) (~((uint128_t) 0ULL)))
#define UINT128_MIN ((uint128_t) 0ULL)
