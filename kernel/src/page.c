#include <stddef.h>
#include <stdint.h>
#include <system/memfuncs.h>
#include <system/pmm.h>
#include <system/printf.h>

#define PAGING_X86_64_PRESENT (0x1L)
#define PAGING_X86_64_WRITE (0x2L)
#define PAGING_X86_64_USER_ALLOWED (0x4L)
#define PAGING_X86_64_EXECUTE_DISABLE (1L << 63)
#define PAGING_X86_64_PHYS_MASK (0x00FFFFFFF000)
#define PAGING_X86_64_PS (1L << 7)
#define PAGING_X86_64_UNCACHABLE (1L << 4)
#define PAGE_SIZE 4096
#define CONVERT_ADDR(v) (v + o)

uint64_t local_offset = 0;

typedef struct {
    uint64_t entries[512];
} __attribute__((packed)) PageTable;

unsigned long get_cr3(void) {
    unsigned long cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}


void paging_map_cr3(uint64_t phys, uint64_t virt, uint64_t permissions) {
    uint64_t offset = local_offset;
    uint16_t level1 = (virt >> 12) & 0x1ff;
    uint16_t level2 = (virt >> 21) & 0x1ff;
    uint16_t level3 = (virt >> 30) & 0x1ff;
    uint16_t level4 = (virt >> 39) & 0x1ff;

    PageTable *pml4 = (void*)((uint64_t) get_cr3() + (uint64_t) local_offset);

    uint64_t in_between_perms = PAGING_X86_64_PRESENT | PAGING_X86_64_WRITE;
    if (permissions & PAGING_X86_64_USER_ALLOWED) {
        in_between_perms |= PAGING_X86_64_USER_ALLOWED;
    }

    uint64_t level4_entry = (pml4->entries[level4]);
    if (!(level4_entry & PAGING_X86_64_PRESENT)) {
        void *mem = pmm_alloc_page();
        memset(mem, 0, PAGE_SIZE);
        uint64_t new_entry = ((uint64_t) (mem) &PAGING_X86_64_PHYS_MASK) | in_between_perms;
        pml4->entries[level4] = new_entry;
        level4_entry = new_entry;
        level4_entry -= offset;
    }
    PageTable *pdpt = (PageTable *) ((level4_entry & PAGING_X86_64_PHYS_MASK) + offset);

    uint64_t level3_entry = (pdpt->entries[level3]);
    if (!(level3_entry & PAGING_X86_64_PRESENT)) {
        void *mem = pmm_alloc_page();
        memset(mem, 0, PAGE_SIZE);
        uint64_t new_entry = ((uint64_t) (mem) &PAGING_X86_64_PHYS_MASK) | in_between_perms;
        pdpt->entries[level3] = new_entry;
        level3_entry = new_entry;
        level3_entry -= offset;
    }
    PageTable *pd = (PageTable *) (((level3_entry & PAGING_X86_64_PHYS_MASK) + offset));

    uint64_t level2_entry = (pd->entries[level2]);
    if (!(level2_entry & PAGING_X86_64_PRESENT)) {
        void *mem = pmm_alloc_page();
        memset(mem, 0, PAGE_SIZE);
        uint64_t new_entry = ((uint64_t) mem & PAGING_X86_64_PHYS_MASK) | in_between_perms;
        pd->entries[level2] = new_entry;
        level2_entry = new_entry;
        level2_entry -= offset;
    }
    PageTable *pt = (PageTable *) (((level2_entry & PAGING_X86_64_PHYS_MASK) + offset));

    pt->entries[level1] = ((uint64_t) phys & PAGING_X86_64_PHYS_MASK) | permissions;
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

void paging_unmap_cr3(uint64_t virt) {
    paging_map_cr3(0, virt, 0);
}

uint64_t add_offset(uint64_t cr3) {
    return cr3 + local_offset;
}

void init_paging(uint64_t offset) {
    local_offset = offset;
    paging_map_cr3(0x1000, 0x2000, PAGING_X86_64_PRESENT | PAGING_X86_64_EXECUTE_DISABLE);
}
