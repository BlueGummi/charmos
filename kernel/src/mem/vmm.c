#include <limine.h>
#include <pmm.h>
#include <printf.h>
#include <stdint.h>
#include <string.h>
#include <vmalloc.h>
#include <vmm.h>

struct page_table *kernel_pml4 = NULL;
uintptr_t kernel_pml4_phys = 0;
static uint64_t hhdm_offset = 0;

uint64_t sub_offset(uint64_t a) {
    return a - hhdm_offset;
}
void vmm_offset_set(uint64_t o) {
    vmalloc_set_offset(o);
    hhdm_offset = o;
}
unsigned long get_cr3(void) {
    unsigned long cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void vmm_copy_kernel_mappings(uintptr_t new_virt_base) {
    extern uint64_t __stext[], __etext[];
    uintptr_t old_virt_start = (uintptr_t) __stext;
    uintptr_t old_virt_end = (uintptr_t) __etext;

    for (uintptr_t old_virt = old_virt_start; old_virt < old_virt_end;
         old_virt += PAGE_SIZE) {

        uintptr_t phys = SUB_HHDM_OFFSET(old_virt);
        if (phys == (uintptr_t) -1)
            continue;

        uintptr_t new_virt = new_virt_base + (old_virt - old_virt_start);

        uint64_t flags = PAGING_PRESENT;

        if (old_virt >= (uintptr_t) (__stext + 0x1000) &&
            old_virt < (uintptr_t) (__stext + 0xb000)) {
            flags |= PAGING_WRITE;
        }

        vmm_map_page(new_virt, phys, flags);
    }
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

/*  extern uint64_t __stext[], __etext[];
    extern uint64_t __srodata[], __erodata[];
    extern uint64_t __sdata[], __edata[];
    extern uint64_t __sbss[], __ebss[];*/
    extern uint64_t __slimine_requests[], __elimine_requests[];

    for (uintptr_t virt = (uintptr_t) __slimine_requests;
         virt < (uintptr_t) __elimine_requests; virt += PAGE_SIZE) {
        uintptr_t phys = (uintptr_t) pmm_alloc_page(false);
        vmm_map_page(virt, phys, PT_KERNEL_RW);
    }
    vmm_copy_kernel_mappings(0xffffffffc0000000);
    vmm_bitmap_init(0xffff800000000000, 0x100000); // TODO: Make this dynamic
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
