#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t time_t;
typedef uint32_t inode_t;
typedef uint16_t mode_t;
typedef uint32_t gid_t;
typedef uint32_t uid_t;
typedef atomic_uint refcount_t;
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;
typedef uint64_t core_t; /* CPU Core number */
typedef uint64_t cpumask_t;
typedef uint64_t pte_t;
typedef uint64_t page_flags_t;
