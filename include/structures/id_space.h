/* @title: ID Allocator (id_space) */
#pragma once
#include <stdint.h>
#include <structures/rbt.h>
#include <sync/spinlock.h>

#define ID_RANGE_RESERVE_COUNT 128

struct id_range {
    struct rbt_node node;
    uint64_t start;
    uint64_t length;
    struct id_range *next;
};

struct id_space {
    struct rbt tree;
    struct spinlock lock;
    struct id_range reserve_pool[ID_RANGE_RESERVE_COUNT];
    struct id_range *reserve_free;
};

#define ID_SPACE_INIT                                                          \
    (struct id_space) {                                                        \
        .reserve_free = NULL                                                   \
    }

struct id_space *id_space_init(uint64_t max_id);

void id_space_destroy(struct id_space *is);

uint64_t id_space_alloc(struct id_space *is);

void id_space_free(struct id_space *is, uint64_t id);

uint64_t id_space_alloc_range(struct id_space *is, uint64_t count);

void id_space_free_range(struct id_space *is, uint64_t start, uint64_t count);
