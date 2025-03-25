#include <stdbool.h>
#include <stdint.h>
#include <system/memfuncs.h>
#include <system/pmm.h>
#include <system/printf.h>
#include <system/vmm.h>
extern uint64_t hhdm_offset;

#define BITMAP_SIZE (1 << 20) // four giggybites
#define BITS_PER_ENTRY (sizeof(uint64_t) * 8)

typedef struct {
    uint64_t *bitmap;
    uintptr_t base_address;
    size_t total_pages;
    size_t free_pages;
} VmmBitmapAllocator;

static VmmBitmapAllocator vmm_allocator;

void vmm_bitmap_init(uintptr_t base_address, size_t total_pages) {
    size_t bitmap_pages = (total_pages + BITS_PER_ENTRY - 1) / BITS_PER_ENTRY;
    bitmap_pages = (bitmap_pages * sizeof(uint64_t) + PAGE_SIZE - 1) / PAGE_SIZE;

    vmm_allocator.bitmap = (uint64_t *) pmm_alloc_pages(bitmap_pages);
    memset(vmm_allocator.bitmap, 0, bitmap_pages * PAGE_SIZE);
    vmm_allocator.base_address = base_address;
    vmm_allocator.total_pages = total_pages;
    vmm_allocator.free_pages = total_pages;
    uintptr_t bitmap_start = (uintptr_t) vmm_allocator.bitmap /* - hhdm_offset*/;
    for (size_t i = 0; i < bitmap_pages; i++) {
        size_t page_idx = (bitmap_start + i * PAGE_SIZE - base_address) / PAGE_SIZE;
        vmm_allocator.bitmap[page_idx / BITS_PER_ENTRY] |= (1ULL << (page_idx % BITS_PER_ENTRY));
        vmm_allocator.free_pages--;
    }
}

static bool bitmap_test_bit(size_t index) {
    return (vmm_allocator.bitmap[index / BITS_PER_ENTRY] & (1ULL << (index % BITS_PER_ENTRY))) != 0;
}

static void bitmap_set_bit(size_t index) {
    vmm_allocator.bitmap[index / BITS_PER_ENTRY] |= (1ULL << (index % BITS_PER_ENTRY));
}

static void bitmap_clear_bit(size_t index) {
    vmm_allocator.bitmap[index / BITS_PER_ENTRY] &= ~(1ULL << (index % BITS_PER_ENTRY));
}

void *vmm_alloc_pages(size_t count) {
    if (count == 0 || count > vmm_allocator.free_pages) {
        return 0;
    }

    size_t consecutive = 0;
    size_t start_index = 0;

    for (size_t i = 0; i < vmm_allocator.total_pages; i++) {
        if (!bitmap_test_bit(i)) {
            if (consecutive == 0) {
                start_index = i;
            }
            consecutive++;

            if (consecutive == count) {
                for (size_t j = 0; j < count; j++) {
                    bitmap_set_bit(start_index + j);
                }
                vmm_allocator.free_pages -= count;
                uint64_t address = vmm_allocator.base_address + start_index * PAGE_SIZE;
                for (size_t k = 0; k < count; k++) {
                    uintptr_t virt = (uintptr_t) address + (PAGE_SIZE * k);
                    uintptr_t phys = sub_offset((uint64_t) pmm_alloc_page());
                    vmm_map_page(virt, phys, PAGING_PRESENT | PAGING_WRITE);
                }
                return (void *) address;
            }
        } else {
            consecutive = 0;
        }
    }

    return 0;
}

void vmm_free_pages(void *address, size_t count) {
    if ((uint64_t) address < vmm_allocator.base_address || count == 0) {
        return;
    }

    size_t start_index = ((uint64_t) address - vmm_allocator.base_address) / PAGE_SIZE;

    if (start_index + count > vmm_allocator.total_pages) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        vmm_unmap_page(((uintptr_t) address) * i);
        if (bitmap_test_bit(start_index + i)) {
            bitmap_clear_bit(start_index + i);
            vmm_allocator.free_pages++;
        }
    }
}

size_t vmm_get_free_pages(void) {
    return vmm_allocator.free_pages;
}

size_t vmm_get_total_pages(void) {
    return vmm_allocator.total_pages;
}
