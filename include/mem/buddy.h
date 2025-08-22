#include <stdbool.h>
#include <stdint.h>
#include <types/types.h>

#define MAX_ORDER 20
#pragma once

struct buddy_page {
    uint64_t pfn;
    uint64_t order;
    struct buddy_page *next;
    struct free_area *free_area;
    bool is_free;
};

struct free_area {
    struct buddy_page *next;
    uint64_t nr_free;
};

extern struct free_area buddy_free_area[MAX_ORDER];
extern struct buddy_page *buddy_page_array;

paddr_t buddy_alloc_pages(uint64_t count);
void buddy_free_pages(paddr_t addr, uint64_t count);
struct limine_memmap_entry;
void buddy_add_entry(struct limine_memmap_entry *entry);
