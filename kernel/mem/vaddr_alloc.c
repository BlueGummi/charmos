#include <console/panic.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vaddr_alloc.h>
#include <misc/align.h>
#include <string.h>

#define VASRANGE_PER_PAGE (PAGE_SIZE / sizeof(struct vas_range))

static void vasrange_refill(struct vas_space *space) {
    uintptr_t phys = pmm_alloc_page(ALLOC_FLAGS_NONE);
    if (!phys)
        k_panic("OOM allocating vas_range page");

    uintptr_t virt = phys + global.hhdm_offset;
    struct vas_range *ranges = (struct vas_range *) virt;

    for (uint64_t i = 0; i < VASRANGE_PER_PAGE; i++) {
        ranges[i].next_free = space->freelist;
        space->freelist = &ranges[i];
    }
}

struct vas_range *vasrange_alloc(struct vas_space *space) {
    if (!space->freelist)
        vasrange_refill(space);

    struct vas_range *r = space->freelist;
    space->freelist = r->next_free;
    return r;
}

void vasrange_free(struct vas_space *space, struct vas_range *r) {
    r->next_free = space->freelist;
    space->freelist = r;
}

struct vas_space *vas_space_bootstrap(vaddr_t base, vaddr_t limit) {
    uintptr_t phys = pmm_alloc_page(ALLOC_FLAGS_NONE);
    if (!phys)
        k_panic("OOM creating vas_space");

    uintptr_t virt = phys + global.hhdm_offset;

    struct vas_space *vas = (struct vas_space *) virt;

    memset(vas, 0, sizeof(*vas));
    vas->base = base;
    vas->limit = limit;
    vas->tree.root = NULL;
    spinlock_init(&vas->lock);

    return vas;
}

struct vas_space *vas_space_init(vaddr_t base, vaddr_t limit) {
    struct vas_space *vas = kzalloc(sizeof(struct vas_space));
    if (!vas)
        return NULL;

    vas->base = base;
    vas->limit = limit;
    vas->tree.root = NULL;
    spinlock_init(&vas->lock);
    return vas;
}

vaddr_t vas_alloc(struct vas_space *vas, size_t size, size_t align) {
    enum irql irql = vas_space_lock_irq_disable(vas);
    vaddr_t prev_end = ALIGN_UP(vas->base, align);

    struct rbt_node *node = rbt_min(&vas->tree);
    while (node) {
        struct vas_range *vr = rbt_entry(node, struct vas_range, node);

        if (prev_end + size <= vr->start) {
            struct vas_range *new_range = vasrange_alloc(vas);
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
        new_range->start = prev_end;
        new_range->length = size;
        new_range->node.data = new_range->start;
        rbt_insert(&vas->tree, &new_range->node);
        vas_space_unlock(vas, irql);
        return prev_end;
    }

    vas_space_unlock(vas, irql);
    return 0;
}

void vas_free(struct vas_space *vas, vaddr_t addr) {
    enum irql irql = vas_space_lock_irq_disable(vas);

    struct rbt_node *node = vas->tree.root;
    while (node) {
        struct vas_range *vr = rbt_entry(node, struct vas_range, node);

        if (addr < vr->start) {
            node = node->left;
        } else if (addr > vr->start) {
            node = node->right;
        } else {
            rbt_remove(&vas->tree, vr->node.data);
            vasrange_free(vas, vr);
            vas_space_unlock(vas, irql);
            return;
        }
    }

    vas_space_unlock(vas, irql);
    bool invalid_free_happened = true;
    kassert(invalid_free_happened == true);
}
