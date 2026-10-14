#include <global.h>
#include <mem/alloc.h>
#include <mem/domain.h>
#include <mem/hhdm.h>
#include <mem/movealloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>
#include <thread/dpc.h>

#include "mem/slab/internal.h"

extern struct movealloc_callback_node __skernel_movealloc_callbacks[];
extern struct movealloc_callback_node __ekernel_movealloc_callbacks[];

void movealloc_init_chain(void) {
    list_head_init(&global.movealloc_chain.list);
    struct movealloc_callback_node *start = __skernel_movealloc_callbacks;
    struct movealloc_callback_node *end = __ekernel_movealloc_callbacks;
    for (struct movealloc_callback_node *n = start; n < end; n++) {
        list_node_init(&n->list);
        list_head_push_back(&global.movealloc_chain.list, &n->list);
    }
}

void movealloc_exec_all(void) {
    movealloc_init_chain();
    struct list_node *iter;
    list_for_each(iter, &global.movealloc_chain.list) {
        struct movealloc_callback_node *node =
            movealloc_callback_node_from_list_node(iter);
        node->callback(node->a, node->b);
    }
}

static void change_slab_backing_page(void *ptr) {
    if (valloc_ptr_in_slab_validate(ptr))
        panic("Moved allocations cannot come from slab");
}

void movealloc_full(domain_id_t new_domain, void *ptr, struct vmm_request rq) {
    if (global.current_bootstage >= BOOTSTAGE_COMPLETE)
        panic("movealloc cannot be called after boot completes");

    struct domain *d = global.domains[new_domain];
    size_t size = vsize(ptr);
    size_t pages = PAGES_NEEDED_FOR(size);
    vaddr_t aligned_down = PAGE_ALIGN_DOWN(ptr);
    paddr_t phys_addrs[pages];

    struct vmm_request remap_rq = rq;
    remap_rq.page_flags = PAGE_WRITE | PAGE_PRESENT;
    remap_rq.page_size = VMM_MAP_PAGE_SIZE_4KB;
    remap_rq.vmm_flags |= VMM_FLAG_MODIFY_LEAF;

    for (size_t i = 0; i < pages; i++) {
        vaddr_t vaddr = aligned_down + i * PAGE_SIZE;
        paddr_t paddr = vmm_get_phys_full(vaddr, rq);
        paddr_t new_phys = domain_alloc_from_domain(d, 1);
        if (!new_phys)
            panic("movealloc failed!");

        vaddr_t new_virt = hhdm_paddr_to_vaddr(new_phys);
        void *pvaddr = (void *) vaddr;
        void *pnew_virt = (void *) new_virt;
        memcpy(pnew_virt, pvaddr, PAGE_SIZE);
        vmm_map_page_full(vaddr, new_phys, remap_rq);
        enforce(vmm_get_phys_full(vaddr, rq) == new_phys);

        phys_addrs[i] = paddr;
    }

    for (size_t i = 0; i < pages; i++) {
        pmm_free_page(phys_addrs[i]);
    }

    change_slab_backing_page(ptr);
}
