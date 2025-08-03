#pragma once
#include <stdint.h>

struct minheap_node {
    uint64_t key;
    uint32_t index;
};

struct minheap {
    struct minheap_node **nodes;
    uint32_t capacity;
    uint32_t size;
};
