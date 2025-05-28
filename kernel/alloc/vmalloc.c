#include <pmm.h>
#include <printf.h>
#include <spin_lock.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vmalloc.h>
#include <vmm.h>

// TODO: realloc

uint64_t hhdm_offset;

static struct vmalloc_bitmap vmm_allocator;

static struct spinlock vmalloc_lock = SPINLOCK_INIT;

void vmalloc_set_offset(uint64_t o) {
    hhdm_offset = o;
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
 * Initialize the bitmap for the VMM with a base address of the map
 * and the number of pages it will map
 */
void vmm_bitmap_init(uintptr_t base_address, size_t total_pages) {

    size_t bitmap_pages = (total_pages + BITS_PER_ENTRY - 1) / BITS_PER_ENTRY;

    bitmap_pages =
        (bitmap_pages * sizeof(uint64_t) + PAGE_SIZE - 1) / PAGE_SIZE;

    vmm_allocator.bitmap = (uint64_t *) pmm_alloc_pages(bitmap_pages, true);
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

static bool map_location = true;

/*
 * Helper function once a viable start address in the map is found
 */
static void *vmm_start_idx_alloc(const size_t count, size_t start_index) {

    for (size_t j = 0; j < count; j++) {
        bitmap_set_bit(start_index + j);
    }

    vmm_allocator.free_pages -= count;

    uintptr_t address = vmm_allocator.base_address + start_index * PAGE_SIZE;

    for (size_t k = 0; k < count; k++) {

        uintptr_t virt = address + (k * PAGE_SIZE);
        uintptr_t phys = 0;
        if (map_location)
            phys = (uintptr_t) pmm_alloc_page(false);

        // Allocation failed
        if (phys == (uintptr_t) -1) {

            for (size_t l = 0; l < k; l++) {
                pmm_free_pages((void *) vmm_get_phys(address + (l * PAGE_SIZE)),
                               count, false);
                vmm_unmap_page(address + (l * PAGE_SIZE));
            }

            for (size_t m = 0; m < count; m++) {
                bitmap_clear_bit(start_index + m);
            }

            vmm_allocator.free_pages += count;
            return NULL;
        }
        if (map_location)
            vmm_map_page(virt, phys, PT_KERNEL_RW);
    }
    return (void *) address;
}

/*
 * Allocate `count` consecutive pages
 */
void *vmm_alloc_pages(const size_t count) {
    if (count == 0 || count > vmm_allocator.free_pages) {
        return NULL;
    }

    spin_lock(&vmalloc_lock);

    size_t consecutive = 0;
    size_t start_index = 0;

    for (size_t i = 0; i < vmm_allocator.total_pages; i++) {

        if (!bitmap_test_bit(i)) {

            if (consecutive == 0) {
                start_index = i;
            }
            consecutive++;

            if (consecutive == count) {

                spin_unlock(&vmalloc_lock);

                return vmm_start_idx_alloc(count, start_index);
            }

        } else {

            consecutive = 0;
        }
    }

    spin_unlock(&vmalloc_lock);

    return NULL;
}

/*
 * Free `count` pages, starting from `addr`
 */
void vmm_free_pages(void *addr, size_t count) {
    if ((uintptr_t) addr < vmm_allocator.base_address || count == 0) {
        // Freeing zero pages, nuh uh
        return;
    }

    spin_lock(&vmalloc_lock);

    const uintptr_t address = (uintptr_t) addr;
    const uintptr_t phys = vmm_get_phys(address);

    size_t start_index = (address - vmm_allocator.base_address) / PAGE_SIZE;

    if (start_index + count > vmm_allocator.total_pages) {
        return;
    }

    if (map_location) {
        if (phys != (uintptr_t) -1) {
            pmm_free_pages((void *) phys, count, false);
        }
    }

    for (size_t i = 0; i < count; i++) {

        uintptr_t virt = address + (i * PAGE_SIZE);
        if (map_location)
            vmm_unmap_page(virt);

        if (bitmap_test_bit(start_index + i)) {

            bitmap_clear_bit(start_index + i);
            vmm_allocator.free_pages++;
        }
    }

    spin_unlock(&vmalloc_lock);
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;
    return vmm_alloc_pages((size + PAGE_SIZE - 1) / PAGE_SIZE);
}

void kfree(void *addr, size_t size) {
    if (size == 0 || addr == 0)
        return;
    vmm_free_pages(addr, (size + PAGE_SIZE - 1) / PAGE_SIZE);
}

void *kzalloc(size_t size) {
    if (size == 0)
        return NULL;
    void *ret = vmm_alloc_pages((size + PAGE_SIZE - 1) / PAGE_SIZE);
    memset(ret, 0, size);
    return ret;
}

void set_map_location() {
    map_location = true;
}

void unset_map_location() {
    map_location = false;
}

void vmm_print_memory_status() {
    size_t total_pages = vmm_allocator.total_pages;
    size_t free_pages = vmm_allocator.free_pages;
    size_t allocated_pages = total_pages - free_pages;

    k_printf("VMM Memory Status:\n");
    k_printf("  Total Pages: %zu\n", total_pages);
    k_printf("  Free Pages: %zu\n", free_pages);
    k_printf("  Allocated Pages: %zu\n", allocated_pages);
    k_printf("  Memory Usage: %d%%\n", (allocated_pages * 100) / total_pages);

    k_printf("\nMemory Segments (contiguous):\n");

    if (total_pages == 0) {
        k_printf("  (No pages managed)\n");
        return;
    }

    size_t segment_start = 0;
    bool segment_state = bitmap_test_bit(0);

    for (size_t i = 1; i <= total_pages; i++) {
        bool current_state =
            (i < total_pages) ? bitmap_test_bit(i) : !segment_state;

        if (current_state != segment_state) {
            uintptr_t start_addr =
                vmm_allocator.base_address + segment_start * PAGE_SIZE;
            uintptr_t end_addr = vmm_allocator.base_address +
                                 (i - 1) * PAGE_SIZE + PAGE_SIZE - 1;

            k_printf("  %c: 0x%016lx - 0x%016lx (%zu pages)\n",
                     segment_state ? 'A' : 'F', (unsigned long) start_addr,
                     (unsigned long) end_addr, i - segment_start);

            segment_start = i;
            segment_state = current_state;
        }
    }

    k_printf("\n");
}
