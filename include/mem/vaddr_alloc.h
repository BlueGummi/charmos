#pragma once
#include <misc/rbt.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <types/types.h>

struct vas_range {
    struct rbt_node node;
    vaddr_t start;
    size_t length;
};

struct vas_space {
    struct spinlock lock;
    struct rbt *tree;
    vaddr_t base;
    vaddr_t limit;
};

struct vas_space *vas_space_init(vaddr_t base, vaddr_t limit);
void vas_free(struct vas_space *vas, vaddr_t addr);
vaddr_t vas_alloc(struct vas_space *vas, size_t size, size_t align);

static inline bool vas_space_lock(struct vas_space *vs) {
    return spin_lock(&vs->lock);
}

static inline void vas_space_unlock(struct vas_space *vs, bool iflag) {
    spin_unlock(&vs->lock, iflag);
}
