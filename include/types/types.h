/* @title: Type aliases */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int8_t cpu_perf_t;
typedef uint64_t time_t;
typedef uint32_t inode_t;
typedef uint16_t mode_t;
typedef uint32_t gid_t;
typedef uint32_t uid_t;
typedef _Atomic uint32_t refcount_t;
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;
typedef int64_t cpu_id_t; /* -1 represents "no CPU" */
typedef uint64_t pte_t;
typedef uint64_t page_flags_t;
typedef uint32_t thread_prio_t;
typedef int32_t fx16_16_t;
typedef int32_t nice_t;
