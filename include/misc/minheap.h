#pragma once
#include <stdint.h>
#define MINHEAP_INIT_CAP 64

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
struct minheap_node *minheap_peek(struct minheap *heap);
struct minheap_node *minheap_pop(struct minheap *heap);
uint32_t minheap_size(struct minheap *heap);
