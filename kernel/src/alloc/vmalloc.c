#include <stdbool.h>
#include <stdint.h>
#include <memfuncs.h>
#include <pmm.h>
#include <printf.h>
#include <vmm.h>

extern uint64_t hhdm_offset;
extern void *kernel_pml4;

#define BITS_PER_ENTRY (sizeof(uint64_t) * 8)

/*
 * The VMM bitmap allocator, containing page counts, free page counts, addresses
 * for the first index, and the bitmap address
 */
typedef struct {
    uint64_t *bitmap;
    uintptr_t base_address;
    size_t total_pages;
    size_t free_pages;
} VmmBitmapAllocator;

static VmmBitmapAllocator vmm_allocator;

/*
 * Return the physical address of a mapped page
 */
uintptr_t vmm_get_phys(uintptr_t virt) {
    uint64_t L1 = (virt >> 12) & 0x1FF;
    uint64_t L2 = (virt >> 21) & 0x1FF;
    uint64_t L3 = (virt >> 30) & 0x1FF;
    uint64_t L4 = (virt >> 39) & 0x1FF;

    PageTable *current_table = kernel_pml4;

    PageTableEntry *entry = &current_table->entries[L4];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L3];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L2];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;
    current_table = (PageTable *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L1];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;

    return (*entry & PAGING_PHYS_MASK) + (virt & 0xFFF);
}

/*
 * Initialize the bitmap for the VMM with a base address of the map
 * and the number of pages it will map
 */
void vmm_bitmap_init(uintptr_t base_address, size_t total_pages) {
    size_t bitmap_pages = (total_pages + BITS_PER_ENTRY - 1) / BITS_PER_ENTRY;
    bitmap_pages =
        (bitmap_pages * sizeof(uint64_t) + PAGE_SIZE - 1) / PAGE_SIZE;

    vmm_allocator.bitmap = (uint64_t *) pmm_alloc_pages(bitmap_pages);
    memset(vmm_allocator.bitmap, 0, bitmap_pages * PAGE_SIZE);
    vmm_allocator.base_address = base_address;
    vmm_allocator.total_pages = total_pages;
    vmm_allocator.free_pages = total_pages;
    uintptr_t bitmap_start = (uintptr_t) vmm_allocator.bitmap;
    for (size_t i = 0; i < bitmap_pages; i++) {
        size_t page_idx =
            (bitmap_start + i * PAGE_SIZE - base_address) / PAGE_SIZE;
        vmm_allocator.bitmap[page_idx / BITS_PER_ENTRY] |=
            (1ULL << (page_idx % BITS_PER_ENTRY));
        vmm_allocator.free_pages--;
    }
}

/*
 * Return 1 if the bit at an index is on, else 0 in the VMM bitmap
 */
static bool bitmap_test_bit(size_t index) {
    return (vmm_allocator.bitmap[index / BITS_PER_ENTRY] &
            (1ULL << (index % BITS_PER_ENTRY))) != 0;
}
/*
 * Set a bit to 1 at an index in the VMM bitmap
 */
static void bitmap_set_bit(size_t index) {
    vmm_allocator.bitmap[index / BITS_PER_ENTRY] |=
        (1ULL << (index % BITS_PER_ENTRY));
}
/*
 * Set a bit to 0 at an index in the VMM bitmap
 */
static void bitmap_clear_bit(size_t index) {
    vmm_allocator.bitmap[index / BITS_PER_ENTRY] &=
        ~(1ULL << (index % BITS_PER_ENTRY));
}

/*
 * Allocate `count` consecutive pages
 */
void *vmm_alloc_pages(const size_t count) {
    if (count == 0 || count > vmm_allocator.free_pages) {
        return NULL;
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

                uintptr_t address =
                    vmm_allocator.base_address + start_index * PAGE_SIZE;
                for (size_t k = 0; k < count; k++) {
                    uintptr_t virt = address + (k * PAGE_SIZE);
                    uintptr_t phys = (uintptr_t) pmm_alloc_page(false);

                    // Allocation failed
                    if (phys == (uintptr_t) -1) {
                        for (size_t l = 0; l < k; l++) {
                            pmm_free_pages(
                                (void *) (vmm_get_phys(address +
                                                       (l * PAGE_SIZE)) +
                                          hhdm_offset),
                                count);
                            vmm_unmap_page(address + (l * PAGE_SIZE));
                        }
                        for (size_t m = 0; m < count; m++) {
                            bitmap_clear_bit(start_index + m);
                        }
                        vmm_allocator.free_pages += count;
                        return NULL;
                    }
                    vmm_map_page(virt, phys, PT_KERNEL_RW);
                }
                return (void *) address;
            }
        } else {
            consecutive = 0;
        }
    }

    return NULL;
}

/*
 * Free `count` pages, starting from `addr`
 */
void vmm_free_pages(void *addr, size_t count) {
    if ((uintptr_t) addr < vmm_allocator.base_address ||
        count == 0) { // Freeing zero pages, nuh uh
        return;
    }

    const uintptr_t address = (uintptr_t) addr;
    const uintptr_t phys = vmm_get_phys(address);
    size_t start_index = (address - vmm_allocator.base_address) / PAGE_SIZE;

    if (start_index + count > vmm_allocator.total_pages) {
        return;
    }

    if (phys != (uintptr_t) -1) {
        pmm_free_pages((void *) (phys + hhdm_offset), count);
    }
    for (size_t i = 0; i < count; i++) {
        uintptr_t virt = address + (i * PAGE_SIZE);
        vmm_unmap_page(virt);
        if (bitmap_test_bit(start_index + i)) {
            bitmap_clear_bit(start_index + i);
            vmm_allocator.free_pages++;
        }
    }
}
