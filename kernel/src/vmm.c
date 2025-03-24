#include <stdint.h>
#include <system/memfuncs.h>
#include <system/pmm.h>
#include <system/printf.h>
#define PAGING_PRESENT (0x1L)
#define PAGING_WRITE (0x2L)
#define PAGING_USER_ALLOWED (0x4L)
#define PAGING_XD (1L << 63) // E(x)ecute (D)isable
#define PAGING_PHYS_MASK (0x00FFFFFFF000)
#define PAGING_PAGE_SIZE (1L << 7)
#define PAGING_UNCACHABLE (1L << 4)
#define PAGE_SIZE 4096
#define SUB_HHDM_OFFSET(v) (v - hhdm_offset)

/* This needs to fix paging and setup our custom page tables
 * rather than modify existing page tables (which may disappear) as they are in bootloader-reclaimable memory.
 * We need to create types to define page tables, and then setup each table with incrementing addresses.
 *
 * I will attempt to make this as clear as I possibly can 
 * (type aliases, and favoring duplicate types for clarity over reusing uint64_t)
 */

typedef uint64_t PageTableEntry; // Page Table Entry

typedef struct {
    PageTableEntry entries[512];
} __attribute__((packed)) PageTable;

typedef struct {
    union { // Using unions for name aliases
        uint16_t PageGlobalDirectory;
        uint16_t PGD;
        uint16_t L4;
    };
    union {
        uint16_t PageUpperDirectory;
        uint16_t PUD;
        uint16_t L3;
    };
    union {
        uint16_t PageMiddleDirectory;
        uint16_t PMD;
        uint16_t L2;
    };
    union {
        uint16_t PageTableEntry;
        uint16_t PTE;
        uint16_t L1;
    };
    uint16_t offset;
} VirtAddr;

uint64_t hhdm_offset = 0;

void vmm_offset_set(uint64_t o) { // This offset is given by limine.
    // It will indicate how to take "virtual" (offsetted) addresses
    // from pmm_page_alloc and get the physical location of the page.
    hhdm_offset = o;
}

// We will work with 4-level paging for now

/* Takes a virtual address, masks indices and returns
 * a VirtAddr struct with fields to denote segments
 */

VirtAddr vmm_extract_virtaddr(uint64_t address) {
    VirtAddr vaddr = {
        .L4 = (address & ((uint64_t) 0x1ff << 39)) >> 39,
        .L3 = (address & ((uint64_t) 0x1ff << 30)) >> 30,
        .L2 = (address & ((uint64_t) 0x1ff << 21)) >> 21,
        .L1 = (address & ((uint64_t) 0x1ff << 12)) >> 12,
        .offset = (address & ((uint64_t) 0x1ff)),
    };
    return vaddr;
}


