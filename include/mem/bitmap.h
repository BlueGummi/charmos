/* @title: Bitmap allocator */
#pragma once
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <types/types.h>

#define BOOT_BITMAP_SIZE ((1024 * 1024 * 128) / PAGE_SIZE / 8)

extern uint8_t boot_bitmap[BOOT_BITMAP_SIZE];
extern uint8_t *bitmap;
extern uint64_t bitmap_size;

paddr_t bitmap_alloc_pages(uint64_t count, enum alloc_flags f);
void bitmap_free_pages(paddr_t addr, uint64_t count);

__no_sanitize_address static inline void set_bit(uint64_t index) {
    uint64_t byte = index / 8;
    uint8_t mask = 1 << (index % 8);
    if (byte > BOOT_BITMAP_SIZE)
        return;

    __atomic_fetch_or(&bitmap[byte], mask, __ATOMIC_SEQ_CST);
}

__no_sanitize_address static inline void clear_bit(uint64_t index) {
    uint64_t byte = index / 8;
    uint8_t mask = ~(1 << (index % 8));
    if (byte > BOOT_BITMAP_SIZE)
        return;

    __atomic_fetch_and(&bitmap[byte], mask, __ATOMIC_SEQ_CST);
}

__no_sanitize_address static inline bool test_bit(uint64_t index) {
    uint64_t byte = index / 8;
    uint8_t value;
    if (byte > BOOT_BITMAP_SIZE)
        return false;

    __atomic_load(&bitmap[byte], &value, __ATOMIC_SEQ_CST);
    return (value & (1 << (index % 8))) != 0;
}
