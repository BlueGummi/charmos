#include <charmos.h>
#include <mem/alloc.h>
#include <mem/domain.h>
#include <mem/movealloc.h>
#include <mem/vmm.h>
#include <string.h>

#include "mem/slab/internal.h"

extern struct movealloc_callback_node __skernel_movealloc_callbacks[];
extern struct movealloc_callback_node __ekernel_movealloc_callbacks[];

void movealloc_init_chain(void) {
    INIT_LIST_HEAD(&global.movealloc_chain.list);
    struct movealloc_callback_node *start = __skernel_movealloc_callbacks;
    struct movealloc_callback_node *end = __ekernel_movealloc_callbacks;
    for (struct movealloc_callback_node *n = start; n < end; n++) {
        INIT_LIST_HEAD(&n->list);
        list_add_tail(&n->list, &global.movealloc_chain.list);
    }
}

void movealloc_exec_all(void) {
    movealloc_init_chain();
    struct list_head *iter;
    list_for_each(iter, &global.movealloc_chain.list) {
        struct movealloc_callback_node *node =
            movealloc_callback_node_from_list_node(iter);
        node->callback(node->a, node->b);
    }
}

static vaddr_t phys_get_virt(paddr_t phys) {
    return phys + global.hhdm_offset;
}

static void change_slab_backing_page(void *ptr) {
    if (slab_size_to_index(ksize(ptr)) == -1)
        return;

    k_panic("Moved allocations cannot come from slab\n");
}

void movealloc(size_t new_domain, void *ptr) {
    if (global.current_bootstage >= BOOTSTAGE_COMPLETE)
        k_panic("movealloc cannot be called after boot completes\n");

    struct domain *d = global.domains[new_domain];
    size_t size = ksize(ptr);
    size_t pages = PAGES_NEEDED_FOR(size);
    vaddr_t aligned_down = PAGE_ALIGN_DOWN(ptr);

    for (size_t i = 0; i < pages; i++) {
        vaddr_t vaddr = aligned_down + i * PAGE_SIZE;
        paddr_t new_phys = domain_alloc_from_domain(d, 1);
        if (!new_phys)
            k_panic("movealloc failed!\n");

        vaddr_t new_virt = phys_get_virt(new_phys);
        void *pvaddr = (void *) vaddr;
        void *pnew_virt = (void *) new_virt;
        memcpy(pnew_virt, pvaddr, PAGE_SIZE);
        vmm_map_page(vaddr, new_phys, PAGING_WRITE | PAGING_PRESENT);
        kassert(vmm_get_phys(vaddr) == new_phys);
    }

    change_slab_backing_page(ptr);
}
