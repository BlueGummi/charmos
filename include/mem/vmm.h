/* @title: Virtual memory management */
#pragma once
#include <console/printf.h>
#include <errno.h>
#include <limine.h>
#include <mem/demand_page.h>
#include <stdint.h>
#include <types/types.h>

struct page_table;

enum vmm_map_page_size : uint8_t {
    VMM_MAP_PAGE_SIZE_4KB,
    VMM_MAP_PAGE_SIZE_2MB,
    VMM_MAP_PAGE_SIZE_1GB,
};

/* Big bitmap set that is embedded in vmm_map_request */
enum vmm_flags : uint64_t {
    VMM_FLAG_NONE = 0,
    VMM_FLAG_NO_TLB_SHOOTDOWN = 1 << 0,
    VMM_FLAG_USER = 1 << 1,
    VMM_FLAG_MODIFY_LEAF = 1 << 2, /* This flag exists since
                                    * certain calls simply serve
                                    * to modify the leaf (e.g. faulting
                                    * on a demand page and
                                    * mapping in the physical frame)
                                    *
                                    * By default, we panic if the
                                    * leaf is modified, and this
                                    * flag says "don't panic,
                                    * I know what I'm doing" */

    VMM_FLAG_CLEAR_LEAF = 1 << 3, /* Clear all the data (besides the lock bit,
                                   * transiently), zeroing out the leaf */

    VMM_FLAG_HANDLE_PTE_EXISTING = 1 << 4, /* Will return ERR_EXIST
                                            * when used in conjunction
                                            * with MODIFY_LEAF and PRESENT
                                            * is seen on the leaf PTE */
};

/* map/unmap page full get these, everyone else uses it to call into it,
 * wrappers automatically build these, everything's hunky dory! */
struct vmm_map_request {
    struct page_table *pml4;
    vaddr_t virt;
    paddr_t phys;
    size_t len;
    page_flags_t page_flags;

    enum vmm_flags vmm_flags;
    enum vmm_map_page_size page_size;

    /* Internal flags */
    bool is_unmap_internal;
};

void vmm_init(struct limine_memmap_response *memmap,
              struct limine_executable_address_response *xa);

enum errno vmm_map_page_full(struct vmm_map_request *rq);
void vmm_unmap_page_full(struct vmm_map_request *rq);

enum errno vmm_map_aliased(vaddr_t virt, size_t len, paddr_t phys,
                           page_flags_t leaf_flags, enum vmm_flags vflags);
void vmm_unmap_aliased(vaddr_t virt, size_t len, enum vmm_flags vflags);
enum errno vmm_unshare_path(vaddr_t virt, enum vmm_map_page_size leaf_size,
                            enum vmm_flags vflags);

enum errno vmm_map_page_internal(vaddr_t virt, paddr_t phys, page_flags_t flags,
                                 enum vmm_flags vflags,
                                 enum vmm_map_page_size size);
enum errno vmm_map_page_user_internal(struct page_table *pml4, vaddr_t virt,
                                      paddr_t phys, page_flags_t flags,
                                      enum vmm_flags vflags,
                                      enum vmm_map_page_size size);
void vmm_unmap_page_internal(vaddr_t virt, enum vmm_flags vflags,
                             enum vmm_map_page_size size);
enum errno vmm_mark_demand_page_internal(vaddr_t virt,
                                         enum demand_page_flags flags,
                                         enum vmm_map_page_size size);
enum errno vmm_mark_demand_page_user_internal(struct page_table *pml4,
                                              vaddr_t virt,
                                              enum demand_page_flags flags,
                                              enum vmm_map_page_size size);
enum errno vmm_map_demand_page_internal(vaddr_t virt, paddr_t phys,
                                        enum demand_page_flags flags,
                                        enum vmm_map_page_size size);

paddr_t vmm_get_phys_internal(vaddr_t virt, enum vmm_flags flags);
pte_t vmm_get_leaf_pte_internal(vaddr_t virt, enum vmm_flags flags);
void vmm_unmap(void *addr, uint64_t len, enum vmm_flags vflags);
void *vmm_map(paddr_t paddr, vaddr_t vaddr, uint64_t len, uint64_t flags,
              enum vmm_flags vflags);
void *vmm_map_bump_internal(uint64_t addr, uint64_t len, uint64_t flags,
                            enum vmm_flags vflags);
void vmm_unmap_virt(void *addr, uint64_t len, enum vmm_flags vflags);
uintptr_t vmm_make_user_pml4(void);
void vmm_unmap_all_user_pages(struct page_table *pml4, enum vmm_flags vflags);
void vmm_reclaim_page_tables(void);
struct page_table *vmm_phys_to_pml4(paddr_t paddr);

#include <mem/vmm_api_internal.h>
