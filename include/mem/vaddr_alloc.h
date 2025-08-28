#pragma once
#include <misc/rbt.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <types/types.h>

struct vas_range {
    struct vas_range *next_free;
    struct rbt_node node;
    vaddr_t start;
    size_t length;
};

struct vas_space {
    struct spinlock lock;
    struct rbt tree;
    vaddr_t base;
    vaddr_t limit;
};

struct vas_space *vas_space_init(vaddr_t base, vaddr_t limit);
void vas_free(struct vas_space *vas, vaddr_t addr);
vaddr_t vas_alloc(struct vas_space *vas, size_t size, size_t align);
struct vas_space *vas_space_bootstrap(vaddr_t base, vaddr_t limit);
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(vas_space, lock);
