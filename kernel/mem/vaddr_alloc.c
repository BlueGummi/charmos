#include <console/panic.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/vaddr_alloc.h>
#include <misc/align.h>

struct vas_space *vas_space_init(vaddr_t base, vaddr_t limit) {
    struct vas_space *vas = kzalloc(sizeof(struct vas_space));
    if (!vas)
        return NULL;

    vas->base = base;
    vas->limit = limit;
    vas->tree = rbt_create();
    spinlock_init(&vas->lock);
    return vas;
}

vaddr_t vas_alloc(struct vas_space *vas, size_t size, size_t align) {
    bool iflag = vas_space_lock(vas);
    vaddr_t prev_end = ALIGN_UP(vas->base, align);

    struct rbt_node *node = rbt_min(vas->tree);
    while (node) {
        struct vas_range *vr = rbt_entry(node, struct vas_range, node);

        if (prev_end + size <= vr->start) {
            struct vas_range *new_range = kzalloc(sizeof(*new_range));
            new_range->start = prev_end;
            new_range->length = size;
            new_range->node.data = new_range->start;
            rbt_insert(vas->tree, &new_range->node);
            vas_space_unlock(vas, iflag);
            return prev_end;
        }

        prev_end = ALIGN_UP(vr->start + vr->length, align);
        node = rbt_next(node);
    }

    if (prev_end + size <= vas->limit) {
        struct vas_range *new_range = kzalloc(sizeof(*new_range));
        new_range->start = prev_end;
        new_range->length = size;
        new_range->node.data = new_range->start;
        rbt_insert(vas->tree, &new_range->node);
        vas_space_unlock(vas, iflag);
        return prev_end;
    }

    vas_space_unlock(vas, iflag);
    return 0;
}

void vas_free(struct vas_space *vas, vaddr_t addr) {
    bool iflag = vas_space_lock(vas);

    struct rbt_node *node = vas->tree->root;
    while (node) {
        struct vas_range *vr = rbt_entry(node, struct vas_range, node);

        if (addr < vr->start) {
            node = node->left;
        } else if (addr > vr->start) {
            node = node->right;
        } else {
            rbt_remove(vas->tree, vr->node.data);
            kfree(vr);
            vas_space_unlock(vas, iflag);
            return;
        }
    }

    vas_space_unlock(vas, iflag);
    bool invalid_free_happened = true;
    kassert(invalid_free_happened == true);
}
