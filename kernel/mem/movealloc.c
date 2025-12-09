#include <charmos.h>
#include <mem/alloc.h>
#include <mem/domain.h>
#include <mem/movealloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>
#include <thread/dpc.h>

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

void movealloc(size_t new_domain, void *ptr, enum vmm_flags flags) {
    if (global.current_bootstage >= BOOTSTAGE_COMPLETE)
        k_panic("movealloc cannot be called after boot completes\n");

    struct domain *d = global.domains[new_domain];
    size_t size = ksize(ptr);
    size_t pages = PAGES_NEEDED_FOR(size);
    vaddr_t aligned_down = PAGE_ALIGN_DOWN(ptr);
    paddr_t phys_addrs[pages];

    for (size_t i = 0; i < pages; i++) {
        vaddr_t vaddr = aligned_down + i * PAGE_SIZE;
        paddr_t paddr = vmm_get_phys(vaddr, flags);
        paddr_t new_phys = domain_alloc_from_domain(d, 1);
        if (!new_phys)
            k_panic("movealloc failed!\n");

        vaddr_t new_virt = phys_get_virt(new_phys);
        void *pvaddr = (void *) vaddr;
        void *pnew_virt = (void *) new_virt;
        memcpy(pnew_virt, pvaddr, PAGE_SIZE);
        vmm_map_page(vaddr, new_phys, PAGING_WRITE | PAGING_PRESENT, flags);
        kassert(vmm_get_phys(vaddr, flags) == new_phys);

        phys_addrs[i] = paddr;
    }

    if (ptr != smp_core()) {
        for (size_t i = 0; i < pages; i++) {
            pmm_free_page(phys_addrs[i]);
        }
    }

    change_slab_backing_page(ptr);
}

static atomic_uint cores_ran_move_core_dpc = 0;

void move_core_dpc(struct dpc *dpc, void *v) {
    (void) dpc;
    (void) v;
    movealloc(domain_local_id(), smp_core(), VMM_FLAG_NONE);
    atomic_fetch_add(&cores_ran_move_core_dpc, 1);
    tlb_flush();
}

void movealloc_move_all_cores(void) {
    size_t i;
    for_each_cpu_id(i) {
        uint32_t before = atomic_load(&cores_ran_move_core_dpc);
        struct dpc *dpc = dpc_create(move_core_dpc, NULL);
        if (!dpc)
            k_panic("OOM\n");

        dpc_enqueue_on_cpu(i, dpc, DPC_NONE);
        while (atomic_load(&cores_ran_move_core_dpc) == before)
            cpu_relax();

        kfree(dpc, FREE_PARAMS_DEFAULT);
    }
}
