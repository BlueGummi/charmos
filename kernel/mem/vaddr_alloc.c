#include <console/panic.h>
#include <kassert.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/pmm.h>
#include <mem/vaddr_alloc.h>
#include <string.h>

#define VASRANGE_PER_PAGE (PAGE_SIZE / sizeof(struct vas_range))

static size_t vas_range_get_data(struct rbt_node *n) {
    return container_of(n, struct vas_range, node)->start;
}

static int32_t vas_range_cmp(const struct rbt_node *a,
                             const struct rbt_node *b) {
    vaddr_t l = vas_range_get_data((void *) a);
    vaddr_t r = vas_range_get_data((void *) b);
    return (l > r) - (l < r);
}

static void vasrange_refill(struct vas_space *space) {
    SPINLOCK_ASSERT_HELD(&space->lock);

    uintptr_t phys = pmm_alloc_page();
    if (!phys)
        panic("OOM allocating vas_range page");

    uintptr_t virt = phys + global.hhdm_offset;
    struct vas_range *ranges = (struct vas_range *) virt;

    for (uint64_t i = 0; i < VASRANGE_PER_PAGE; i++) {
        INIT_LIST_HEAD(&ranges[i].free_list_node);
        list_add_tail(&ranges[i].free_list_node, &space->freelist);
    }
}

static struct vas_range *vasrange_alloc(struct vas_space *space) {
    SPINLOCK_ASSERT_HELD(&space->lock);

    if (list_empty(&space->freelist))
        vasrange_refill(space);

    struct list_head *pop = list_pop_front_init(&space->freelist);
    struct vas_range *r = container_of(pop, struct vas_range, free_list_node);
    return r;
}

static void vasrange_free(struct vas_space *space, struct vas_range *r) {
    SPINLOCK_ASSERT_HELD(&space->lock);

    list_add_tail(&r->free_list_node, &space->freelist);
}

__no_sanitize_address struct vas_space *vas_space_bootstrap(vaddr_t base,
                                                            vaddr_t limit) {
    uintptr_t phys = pmm_alloc_page();
    if (!phys)
        panic("OOM creating vas_space");

    uintptr_t virt = phys + global.hhdm_offset;
    struct vas_space *vas = (struct vas_space *) virt;

    memset(vas, 0, sizeof(*vas));
    vas->base = base;
    vas->limit = limit;
    INIT_LIST_HEAD(&vas->freelist);

    rbt_init(&vas->tree, vas_range_get_data, vas_range_cmp);
    spinlock_init(&vas->lock);

    /* initial full gap */
    enum irql irql = vas_space_lock(vas);

    struct vas_range *g = vasrange_alloc(vas);
    g->start = base;
    g->length = limit - base;

    rbt_insert(&vas->tree, &g->node);

    vas_space_unlock(vas, irql);
    return vas;
}

struct vas_space *vas_space_init(vaddr_t base, vaddr_t limit) {
    struct vas_space *vas = kzalloc(sizeof(struct vas_space));
    if (!vas)
        return NULL;

    vas->base = base;
    vas->limit = limit;
    INIT_LIST_HEAD(&vas->freelist);

    rbt_init(&vas->tree, vas_range_get_data, vas_range_cmp);
    spinlock_init(&vas->lock);

    enum irql irql = vas_space_lock(vas);

    struct vas_range *g = vasrange_alloc(vas);
    g->start = base;
    g->length = limit - base;

    rbt_insert(&vas->tree, &g->node);

    vas_space_unlock(vas, irql);
    return vas;
}

vaddr_t vas_alloc(struct vas_space *vas, size_t size, size_t align) {
    enum irql irql = vas_space_lock(vas);

    struct rbt_node *node = rbt_min(&vas->tree);

    while (node) {
        struct vas_range *gap = rbt_entry(node, struct vas_range, node);

        vaddr_t aligned = ALIGN_UP(gap->start, align);
        vaddr_t end = aligned + size;

        if (end <= gap->start + gap->length) {
            /* remove current gap */
            rbt_delete(&vas->tree, &gap->node);

            /* left split */
            if (aligned > gap->start) {
                struct vas_range *left = vasrange_alloc(vas);
                left->start = gap->start;
                left->length = aligned - gap->start;
                rbt_insert(&vas->tree, &left->node);
            }

            /* right split */
            if (end < gap->start + gap->length) {
                struct vas_range *right = vasrange_alloc(vas);
                right->start = end;
                right->length = (gap->start + gap->length) - end;
                rbt_insert(&vas->tree, &right->node);
            }

            vasrange_free(vas, gap);

            vas_space_unlock(vas, irql);
            return aligned;
        }

        node = rbt_next(node);
    }

    vas_space_unlock(vas, irql);
    return 0;
}

void vas_free(struct vas_space *vas, vaddr_t addr, size_t size) {
    enum irql irql = vas_space_lock(vas);

    vaddr_t start = addr;
    vaddr_t end = addr + size;

    struct rbt_node *node = vas->tree.root;
    struct vas_range *prev = NULL;
    struct vas_range *next = NULL;

    /* find neighbors */
    while (node) {
        struct vas_range *g = rbt_entry(node, struct vas_range, node);

        if (end <= g->start) {
            next = g;
            node = node->left;
        } else if (start >= g->start + g->length) {
            prev = g;
            node = node->right;
        } else {
            panic("vas_free: overlap/double free");
        }
    }

    /* merge with previous */
    if (prev && prev->start + prev->length == start) {
        start = prev->start;
        size += prev->length;

        rbt_delete(&vas->tree, &prev->node);
        vasrange_free(vas, prev);
    }

    /* merge with next */
    if (next && end == next->start) {
        size += next->length;

        rbt_delete(&vas->tree, &next->node);
        vasrange_free(vas, next);
    }

    /* insert merged gap */
    struct vas_range *g = vasrange_alloc(vas);
    g->start = start;
    g->length = size;

    rbt_insert(&vas->tree, &g->node);

    vas_space_unlock(vas, irql);
}
