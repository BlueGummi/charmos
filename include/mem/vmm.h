/* @title: Virtual memory management */
#include <console/printf.h>
#include <errno.h>
#include <limine.h>
#include <stdint.h>

enum vmm_flags {
    VMM_FLAG_NONE = 0,
    VMM_FLAG_NO_TLB_SHOOTDOWN = 1 << 0,
};

void vmm_init(struct limine_memmap_response *memmap,
              struct limine_executable_address_response *xa);

enum errno vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t pflags,
                        enum vmm_flags vflags);
enum errno vmm_map_2mb_page(uintptr_t virt, uintptr_t phys, uint64_t flags,
                            enum vmm_flags vflags);
void vmm_unmap_2mb_page(uintptr_t virt, enum vmm_flags vflags);
void vmm_unmap_page(uintptr_t virt, enum vmm_flags vflags);
uintptr_t vmm_get_phys(uintptr_t virt, enum vmm_flags vflags);
void *vmm_map_phys(uint64_t addr, uint64_t len, uint64_t flags,
                   enum vmm_flags vflags);
void vmm_unmap_virt(void *addr, uint64_t len, enum vmm_flags vflags);
uintptr_t vmm_make_user_pml4(void);
void vmm_map_page_user(uintptr_t pml4_phys, uintptr_t virt, uintptr_t phys,
                       uint64_t flags, enum vmm_flags vflags);
uintptr_t vmm_get_phys_unsafe(uintptr_t virt);
void vmm_reclaim_page_tables(void);
#pragma once
