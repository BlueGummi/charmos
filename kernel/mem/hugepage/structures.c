#include <kassert.h>
#include <mem/hugepage.h>

#include "internal.h"

void hugepage_tree_insert(struct hugepage_tree *tree, struct hugepage *hp) {
    enum irql irql = hugepage_tree_lock(tree);
    rbt_insert(tree->root_node, &hp->tree_node);
    hugepage_tree_unlock(tree, irql);
}

void hugepage_tree_remove(struct hugepage_tree *tree, struct hugepage *hp) {
    enum irql irql = hugepage_tree_lock(tree);
    rbt_remove(tree->root_node, hp->virt_base);
    hugepage_tree_unlock(tree, irql);
}

/*
 *
 * Core list/minheap logic, insert, delete, peek, pop
 *
 */

void hugepage_core_list_insert(struct hugepage_core_list *list,
                               struct hugepage *hp, bool locked) {
    bool irql;

    if (!locked)
        irql = hugepage_core_list_lock(list);

    kassert(hp->owner_core == list->core_num);
    minheap_insert(list->hugepage_minheap, &hp->minheap_node, hp->virt_base);

    if (!locked)
        hugepage_core_list_unlock(list, irql);
}

void hugepage_return_to_list_internal(struct hugepage *hp) {
    /* Nothing to be done */
    if (hugepage_still_in_core_list(hp))
        return;

    struct hugepage_core_list *hcl = hugepage_get_core_list(hp);
    hugepage_core_list_insert(hcl, hp, false);
}

typedef struct minheap_node *(minheap_fn) (struct minheap *);
static struct hugepage *core_list_do_op(struct hugepage_core_list *hcl,
                                        minheap_fn op) {
    enum irql irql = hugepage_core_list_lock(hcl);

    struct minheap_node *nd = op(hcl->hugepage_minheap);
    if (!nd) {
        hugepage_core_list_unlock(hcl, irql);
        return NULL;
    }
    hugepage_core_list_unlock(hcl, irql);

    return hugepage_from_minheap_node(nd);
}

struct hugepage *hugepage_core_list_peek(struct hugepage_core_list *hcl) {
    return core_list_do_op(hcl, minheap_peek);
}

struct hugepage *hugepage_core_list_pop(struct hugepage_core_list *hcl) {
    return core_list_do_op(hcl, minheap_pop);
}

void hugepage_core_list_remove_hugepage(struct hugepage_core_list *hcl,
                                        struct hugepage *hp, bool locked) {
    bool irql = false;
    if (!locked)
        irql = hugepage_core_list_lock(hcl);

    kassert(hp->owner_core == hcl->core_num);
    minheap_remove(hcl->hugepage_minheap, &hp->minheap_node);

    if (!locked)
        hugepage_core_list_unlock(hcl, irql);
}
