#pragma once
#include <stdatomic.h>
#include <stdint.h>

typedef uint32_t inode_t;
typedef uint16_t mode_t;
typedef uint32_t gid_t;
typedef uint32_t uid_t;
typedef atomic_uint refcount_t;
