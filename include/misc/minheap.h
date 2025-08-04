#pragma once
#include <stddef.h>
#include <stdint.h>
#define MINHEAP_INIT_CAP 64
#define MINHEAP_INDEX_INVALID ((uint32_t) -1)

struct minheap_node {
    uint64_t key;
    uint32_t index;
};

struct minheap {
    struct minheap_node **nodes;
    uint32_t capacity;
    uint32_t size;
};

struct minheap *minheap_create(void);
void minheap_insert(struct minheap *heap, struct minheap_node *node,
                    uint64_t key);
void minheap_remove(struct minheap *heap, struct minheap_node *node);

static inline struct minheap_node *minheap_peek(struct minheap *heap) {
    return heap->size == 0 ? NULL : heap->nodes[0];
}

static inline uint32_t minheap_size(struct minheap *heap) {
    return heap->size;
}

struct minheap_node *minheap_pop(struct minheap *heap);
