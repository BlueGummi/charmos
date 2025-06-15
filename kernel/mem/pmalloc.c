#include "limine.h"
#include <console/printf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PAGE_SIZE 4096U
#define MAX_ORDER 11U
#define MIN_ORDER 0U
#define PAGE_ORDER_BASE 0xFFFFB00000000000ULL 
#define MAX_PAGES (0x100000000U / PAGE_SIZE)
#define PAGE_BLOCK_SIZE(order) (PAGE_SIZE << (order))

struct free_block {
    struct free_block *next;
};

static struct free_block *free_lists[MAX_ORDER + 1];
static uint8_t page_order[MAX_PAGES]; // Tracks order for each block

static uint64_t offset = 0;

static void push_block(uint64_t addr, uint8_t order) {
    struct free_block *block =
        (struct free_block *) (uintptr_t) (addr + offset);
    block->next = free_lists[order];
    free_lists[order] = block;
    page_order[addr / PAGE_SIZE] = order;
}

static void *pop_block(uint8_t order) {
    struct free_block *block = free_lists[order];
    if (!block)
        return NULL;
    free_lists[order] = block->next;
    return block;
}

void pmm_init(uint64_t o, struct limine_memmap_request m) {
    offset = o;

    struct limine_memmap_response *memdata = m.response;
    if (!memdata || !memdata->entries) {
        k_printf("Invalid memory map\n");
        return;
    }

    memset(free_lists, 0, sizeof(free_lists));
    memset(page_order, 0xFF, sizeof(page_order));

    uint64_t total_phys = 0;
    for (uint64_t i = 0; i < memdata->entry_count; i++) {
        struct limine_memmap_entry *entry = memdata->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        total_phys += entry->length;

        uint64_t base = (entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t end = (entry->base + entry->length) & ~(PAGE_SIZE - 1);

        while (base + PAGE_SIZE <= end) {
            uint64_t size = end - base;
            uint8_t order = MAX_ORDER;

            while (order > 0 && (PAGE_BLOCK_SIZE(order) > size ||
                                 (base & (PAGE_BLOCK_SIZE(order) - 1)))) {
                order--;
            }

            push_block(base, order);
            base += PAGE_BLOCK_SIZE(order);
        }
    }
    k_printf("we have %llu bytes of physical memory\n", total_phys);
}

void *pmm_alloc_order(uint8_t order, bool add_offset) {
    if (order > MAX_ORDER)
        return NULL;

    for (uint8_t current = order; current <= MAX_ORDER; current++) {
        void *block = pop_block(current);
        if (!block)
            continue;

        uint64_t block_addr = (uint64_t) (uintptr_t) block - offset;

        while (current > order) {
            current--;
            block_addr += PAGE_BLOCK_SIZE(current);
            push_block(block_addr, current);
            block_addr -= PAGE_BLOCK_SIZE(current);
        }

        page_order[block_addr / PAGE_SIZE] = order;
        return (void *) (block_addr + ((add_offset) ? offset : 0));
    }

    k_printf("I have ran out of RAM\n");
    return NULL;
}

void *pmm_alloc_pages(uint64_t count, bool add_offset) {
    uint8_t order = MIN_ORDER;
    while ((1ULL << order) < count)
        order++;
    return pmm_alloc_order(order, add_offset);
}

void *pmm_alloc_page(bool add_offset){
    return pmm_alloc_pages(1, add_offset);
}

void pmm_free_pages(void *addr, uint64_t count, bool has_offset) {
    if (!addr || count == 0)
        return;

    uintptr_t phys = (uintptr_t) addr - (has_offset ? offset : 0);
    uint64_t page_index = phys / PAGE_SIZE;
    uint8_t order = 0;

    while ((1ULL << order) < count)
        order++;

    while (order <= MAX_ORDER) {
        uint64_t buddy_page = page_index ^ (1ULL << order);
        uintptr_t buddy_addr = buddy_page * PAGE_SIZE;

        if (page_order[buddy_page] != order)
            break;

        struct free_block **prev = &free_lists[order];
        struct free_block *curr = *prev;

        while (curr) {
            if ((uintptr_t) curr - offset == buddy_addr) {
                *prev = curr->next;
                break;
            }
            prev = &curr->next;
            curr = curr->next;
        }

        if (!curr)
            break;

        // Merge
        page_index &= ~(1ULL << order);
        order++;
    }

    push_block(page_index * PAGE_SIZE, order);
}
