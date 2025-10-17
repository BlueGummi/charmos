#include <console/printf.h>
#include <mem/page.h>
#include <stdint.h>

uint64_t sub_offset(uint64_t a);
unsigned long get_cr3(void);
void vmm_init(struct limine_memmap_response *memmap,
              struct limine_executable_address_response *xa);
enum errno vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);
enum errno vmm_map_2mb_page(uintptr_t virt, uintptr_t phys, uint64_t flags);
void vmm_unmap_2mb_page(uintptr_t virt);
void vmm_unmap_page(uintptr_t virt);
uintptr_t vmm_get_phys(uintptr_t virt);
void *vmm_map_phys(uint64_t addr, uint64_t len, uint64_t flags);
void vmm_unmap_virt(void *addr, uint64_t len);
uintptr_t vmm_make_user_pml4(void);
void vmm_map_page_user(uintptr_t pml4_phys, uintptr_t virt, uintptr_t phys,
                       uint64_t flags);
void tlb_shootdown(void *ctx, uint8_t irq, void *rsp);
#pragma once
