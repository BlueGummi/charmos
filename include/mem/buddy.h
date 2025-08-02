#include <stdbool.h>
#include <stdint.h>
#define MAX_ORDER 20
#pragma once

struct buddy_page {
    uint64_t pfn;
    uint64_t order;
    struct buddy_page *next;
    struct free_area *free_area;
};

struct free_area {
    struct buddy_page *next;
    uint64_t nr_free;
};
