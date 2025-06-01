#include <console/printf.h>
#include <limine.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <misc/linker_symbols.h>
#include <spin_lock.h>
#include <stdint.h>
#include <string.h>

struct spinlock vmm_lock = SPINLOCK_INIT;
struct page_table *kernel_pml4 = NULL;
uintptr_t kernel_pml4_phys = 0;
static uint64_t hhdm_offset = 0;
uintptr_t vmm_map_top = VMM_MAP_BASE;

uint64_t sub_offset(uint64_t a) {
    return a - hhdm_offset;
}
void vmm_offset_set(uint64_t o) {
    hhdm_offset = o;
}

uint64_t get_cr3(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void *vmm_map_region(uintptr_t virt_base, uint64_t size, uint64_t flags) {
    void *first = NULL;

    for (uintptr_t virt = virt_base; virt < virt_base + size;
         virt += PAGE_SIZE) {

        uintptr_t phys = (uintptr_t) pmm_alloc_page(false);
        if (virt == virt_base) {
            first = (void *) phys;
        }
        if (phys == (uintptr_t) -1) {
            k_panic("Error: Out of memory mapping region\n");
            return NULL;
        }

        vmm_map_page(virt, phys, flags);
    }

    return first;
}

void vmm_init() {
    kernel_pml4 = (struct page_table *) pmm_alloc_page(true);
    kernel_pml4_phys = (uintptr_t) kernel_pml4 - hhdm_offset;
    memset(kernel_pml4, 0, PAGE_SIZE);

    uintptr_t boot_cr3 = get_cr3();
    struct page_table *boot_pml4 =
        (struct page_table *) (boot_cr3 + hhdm_offset);
    for (size_t i = 0; i < 512; i++) {
        if (boot_pml4->entries[i] & PAGING_PRESENT) {
            kernel_pml4->entries[i] = boot_pml4->entries[i];
        }
    }

    asm volatile("mov %0, %%cr3" : : "r"(kernel_pml4_phys) : "memory");
}

/* This is our virtual memory paging system. We get a virtual address, and a
 * physical address to map it to.
 *
 * We can map a virtual address from anywhere we'd like, but physical addresses
 * must be retrieved elsewhere.
 *
 * Offsets can be subtracted from addresses allocated by pmm_alloc_page
 */
void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    if (virt == 0) {
        k_panic("CANNOT MAP PAGE 0x0!!!\n");
    }

    uint64_t L1 = (virt >> 12) & 0x1FF;
    uint64_t L2 = (virt >> 21) & 0x1FF;
    uint64_t L3 = (virt >> 30) & 0x1FF;
    uint64_t L4 = (virt >> 39) & 0x1FF;

    struct page_table *current_table = kernel_pml4;

    pte_t *entry = &current_table->entries[L4];
    if (!(*entry & PAGING_PRESENT)) {

        struct page_table *new_table =
            (struct page_table *) pmm_alloc_page(true);
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);

        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L3];
    if (!(*entry & PAGING_PRESENT)) {

        struct page_table *new_table =
            (struct page_table *) pmm_alloc_page(true);
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);

        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L2];
    if (!(*entry & PAGING_PRESENT)) {

        struct page_table *new_table =
            (struct page_table *) pmm_alloc_page(true);
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);

        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L1];
    *entry = (phys & PAGING_PHYS_MASK) | flags | PAGING_PRESENT;

    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/*
 * Unmap a singular 4KB page
 */
void vmm_unmap_page(uintptr_t virt) {

    uint64_t L1 = (virt >> 12) & 0x1FF;
    uint64_t L2 = (virt >> 21) & 0x1FF;
    uint64_t L3 = (virt >> 30) & 0x1FF;
    uint64_t L4 = (virt >> 39) & 0x1FF;

    struct page_table *current_table = kernel_pml4;
    pte_t *entry = &current_table->entries[L4];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L3];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L2];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L1];
    *entry &= ~PAGING_PRESENT;
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/*
 * Return the physical address of a mapped page
 */
uintptr_t vmm_get_phys(uintptr_t virt) {

    uint64_t L1 = (virt >> 12) & 0x1FF;
    uint64_t L2 = (virt >> 21) & 0x1FF;
    uint64_t L3 = (virt >> 30) & 0x1FF;
    uint64_t L4 = (virt >> 39) & 0x1FF;

    struct page_table *current_table = kernel_pml4;

    pte_t *entry = &current_table->entries[L4];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;

    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);
    entry = &current_table->entries[L3];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;

    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);
    entry = &current_table->entries[L2];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;

    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);
    entry = &current_table->entries[L1];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;

    return (*entry & PAGING_PHYS_MASK) + (virt & 0xFFF);
}

void *vmm_map_phys(uint64_t addr, uint64_t len) {

    uintptr_t phys_start = PAGE_ALIGN_DOWN(addr);
    uintptr_t offset = addr - phys_start;

    size_t total_len = len + offset;
    size_t total_pages = (total_len + PAGE_SIZE - 1) / PAGE_SIZE;

    if (vmm_map_top + total_pages * PAGE_SIZE > VMM_MAP_LIMIT) {
        return NULL;
    }

    uintptr_t virt_start = vmm_map_top;
    vmm_map_top += total_pages * PAGE_SIZE;

    for (size_t i = 0; i < total_pages; i++) {
        vmm_map_page(virt_start + i * PAGE_SIZE, phys_start + i * PAGE_SIZE,
                     PAGING_PRESENT | PAGING_WRITE);
    }

    return (void *) (virt_start + offset);
}

void vmm_unmap_virt(void *addr, size_t len) {
    uintptr_t virt_addr = (uintptr_t) addr;
    uintptr_t page_offset = virt_addr & (PAGE_SIZE - 1);
    uintptr_t aligned_virt = PAGE_ALIGN_DOWN(virt_addr);

    size_t total_len = len + page_offset;
    size_t total_pages = (total_len + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t i = 0; i < total_pages; i++) {
        vmm_unmap_page(aligned_virt + i * PAGE_SIZE);
    }
}
