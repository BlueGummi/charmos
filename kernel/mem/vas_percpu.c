#include "vas_percpu.h"
#include <kassert.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <string.h>

static inline unsigned vas_percpu_id(void) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return 0;

    return smp_core_id();
}

struct vas_set *vas_set_bootstrap(vaddr_t base, vaddr_t limit, unsigned ncpus) {
    if (ncpus == 0)
        return NULL;

    size_t set_size = sizeof(struct vas_set);
    unsigned pages = PAGES_NEEDED_FOR(set_size);
    uintptr_t page_phys = 0;
    uintptr_t virt = 0;

    page_phys = pmm_alloc_pages(pages, ALLOC_FLAGS_NONE);
    if (!page_phys)
        k_panic("vas percpu alloc failed\n");

    virt = page_phys + global.hhdm_offset;
    struct vas_set *set = (struct vas_set *) virt;
    memset(set, 0, sizeof(*set));

    set->base = base;
    set->limit = limit;
    set->ncpus = ncpus;

    size_t ptrs_size = sizeof(struct vas_space *) * ncpus;
    unsigned ptr_pages = PAGES_NEEDED_FOR(ptrs_size);
    uintptr_t first_ptr_page = pmm_alloc_pages(ptr_pages, ALLOC_FLAGS_NONE);
    if (!first_ptr_page)
        k_panic("vas percpu alloc failed\n");

    uintptr_t ptrs_virt = first_ptr_page + global.hhdm_offset;

    memset((void *) ptrs_virt, 0, ptrs_size);
    set->spaces = (struct vas_space **) ptrs_virt;

    size_t total = (size_t) (limit - base);
    size_t per = total / ncpus;
    set->per_size = per;

    for (unsigned i = 0; i < ncpus; i++) {
        vaddr_t sub_base = base + (vaddr_t) (i * per);
        vaddr_t sub_limit =
            (i == ncpus - 1) ? limit : (sub_base + (vaddr_t) per);
        struct vas_space *vs =
            vas_space_bootstrap_internal(sub_base, sub_limit);
        if (!vs) {
            k_panic("vas percpu alloc failed\n");
        }
        set->spaces[i] = vs;
    }

    return set;
}

struct vas_set *vas_set_init(vaddr_t base, vaddr_t limit, unsigned ncpus) {
    struct vas_set *set = kzalloc(sizeof(*set));
    if (!set)
        return NULL;
    set->base = base;
    set->limit = limit;
    set->ncpus = ncpus;
    set->per_size = (size_t) (limit - base) / ncpus;
    set->spaces = kzalloc(sizeof(struct vas_space *) * ncpus);
    spinlock_init(&set->lock);

    if (!set->spaces) {
        k_panic("OOM\n");
    }
    for (unsigned i = 0; i < ncpus; i++) {
        vaddr_t sb = base + (vaddr_t) (i * set->per_size);
        vaddr_t sl = (i == ncpus - 1) ? limit : (sb + (vaddr_t) set->per_size);
        set->spaces[i] = vas_space_init_internal(sb, sl);
        spinlock_init(&set->spaces[i]->lock);
        if (!set->spaces[i]) {
            k_panic("OOM\n");
        }
    }
    return set;
}

static vaddr_t vas_space_try_alloc_local(struct vas_space *vas, size_t size,
                                         size_t align) {
    enum irql irql = vas_space_lock(vas);

    vaddr_t prev_end = ALIGN_UP(vas->base, align);

    struct rbt_node *node = rbt_min(&vas->tree);
    while (node) {
        struct vas_range *vr = rbt_entry(node, struct vas_range, node);

        if (prev_end + size <= vr->start) {
            struct vas_range *new_range = vasrange_alloc(vas);
            if (!new_range)
                goto out;

            new_range->start = prev_end;
            new_range->length = size;
            new_range->node.data = new_range->start;
            rbt_insert(&vas->tree, &new_range->node);
            vas_space_unlock(vas, irql);
            return prev_end;
        }

        prev_end = ALIGN_UP(vr->start + vr->length, align);
        node = rbt_next(node);
    }

    if (prev_end + size <= vas->limit) {
        struct vas_range *new_range = vasrange_alloc(vas);
        if (!new_range)
            goto out;

        new_range->start = prev_end;
        new_range->length = size;
        new_range->node.data = new_range->start;
        rbt_insert(&vas->tree, &new_range->node);
        vas_space_unlock(vas, irql);
        return prev_end;
    }

out:
    vas_space_unlock(vas, irql);
    return 0;
}

vaddr_t vas_set_alloc(struct vas_set *set, size_t size, size_t align) {
    enum thread_flags flags = scheduler_pin_current_thread();

    enum irql irql = vas_set_lock(set);

    unsigned cpu = vas_percpu_id();
    vaddr_t ret = 0;

    /* try local space first */
    struct vas_space *local = set->spaces[cpu];
    ret = vas_space_try_alloc_local(local, size, align);
    if (ret) {
        vas_set_unlock(set, irql);
        scheduler_unpin_current_thread(flags);
        return ret;
    }

    /* fall back: scan other cpu spaces (simple linear scan) */
    for (unsigned i = 0; i < set->ncpus; i++) {
        unsigned idx = (cpu + 1 + i) % set->ncpus;
        struct vas_space *vs = set->spaces[idx];
        ret = vas_space_try_alloc_local(vs, size, align);
        if (ret)
            break;
    }

    vas_set_unlock(set, irql);
    scheduler_unpin_current_thread(flags);
    return ret; /* 0 on failure */
}

void vas_set_free(struct vas_set *set, vaddr_t addr) {
    return;
    enum thread_flags flags = scheduler_pin_current_thread();
    enum irql set_irql = vas_set_lock(set);

    if (addr < set->base || addr >= set->limit) {
        kassert(false && "vas_set_free: address outside set range");
        return;
    }

    size_t offset = (size_t) (addr - set->base);
    unsigned idx = offset / set->per_size;
    if (idx >= set->ncpus)
        idx = set->ncpus - 1; /* last partition absorbs remainder */

    struct vas_space *owner = set->spaces[idx];
    enum irql irql = vas_space_lock(owner);

    struct rbt_node *node = owner->tree.root;
    while (node) {
        struct vas_range *vr = rbt_entry(node, struct vas_range, node);
        if (addr < vr->start) {
            node = node->left;
        } else if (addr > vr->start) {
            node = node->right;
        } else {
            rbt_remove(&owner->tree, vr->node.data);
            vasrange_free(owner, vr);
            vas_space_unlock(owner, irql);
            vas_set_unlock(set, set_irql);
            scheduler_unpin_current_thread(flags);
            return;
        }
    }

    k_panic("invalid free of 0x%lx\n", addr);
}
