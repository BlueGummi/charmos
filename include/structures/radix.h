/* @title: Radix Tree */
#pragma once
#include <errno.h>
#include <stdint.h>

#define RADIX_BITS 6
#define RADIX_SIZE (1 << RADIX_BITS)
#define RADIX_MASK (RADIX_SIZE - 1)

#define NUM_INSERTS 128
#define NUM_LOOKUPS 32

typedef uint64_t (*radix_key_fn)(const void *item);

struct radix_node {
    struct radix_node *parent;
    void *slots[RADIX_SIZE];
    uint64_t present_mask;
};

struct radix_tree {
    struct radix_node *root;
    uint32_t height;
    radix_key_fn key_fn;
};

int32_t radix_insert(struct radix_tree *tree, void *item);
void *radix_lookup(struct radix_tree *tree, uint64_t key);
void *radix_delete(struct radix_tree *tree, uint64_t key);
struct radix_tree *radix_tree_init(struct radix_tree *r, radix_key_fn kfn,
                                   int height);
