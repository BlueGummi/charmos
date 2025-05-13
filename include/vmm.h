#include <printf.h>
#include <stdint.h>
#define PAGING_PRESENT (0x1L)
#define PAGING_WRITE (0x2L)
#define PAGING_USER_ALLOWED (0x4L)
#define PAGING_XD (1L << 63) // E(x)ecute (D)isable
#define PAGING_PHYS_MASK (0x00FFFFFFF000)
#define PAGING_PAGE_SIZE (1L << 7)
#define PAGING_UNCACHABLE (1L << 4)
#define PAGE_SIZE 4096
#define SUB_HHDM_OFFSET(v) (v - hhdm_offset)
#define PT_KERNEL_RO (PAGING_PRESENT | PAGING_XD)
#define PT_KERNEL_RX (PAGING_PRESENT)
#define PT_KERNEL_RW (PAGING_PRESENT | PAGING_WRITE | PAGING_XD)

typedef uint64_t pte_t; // Page Table Entry

struct page_table {
    pte_t entries[512];
} __attribute__((packed));

uint64_t sub_offset(uint64_t a);
void vmm_offset_set(uint64_t o);
unsigned long get_cr3(void);
void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);
void vmm_init();
void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);
void *vmm_map_region(uintptr_t virt_base, uint64_t size, uint64_t flags);
void vmm_unmap_page(uintptr_t virt);
uintptr_t vmm_get_phys(uintptr_t virt);
extern struct page_table *kernel_pml4;
#pragma once
