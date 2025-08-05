#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/defer.h>
#include <types/refcount.h>

void hugepage_tree_insert(struct hugepage_tree *tree, struct hugepage *hp) {
    bool iflag = hugepage_tree_lock(tree);
    rbt_insert(&tree->root_node, &hp->tree_node);
    hugepage_tree_unlock(tree, iflag);
}

void hugepage_tree_remove(struct hugepage_tree *tree, struct hugepage *hp) {
    bool iflag = hugepage_tree_lock(tree);
    rbt_remove(&tree->root_node, hp->virt_base);
    hugepage_tree_unlock(tree, iflag);
}

/*
 *
 * Core list/minheap logic, insert, delete, peek, pop
 *
 */

void hugepage_core_list_insert(struct hugepage_core_list *list,
                               struct hugepage *hp) {
    bool iflag = hugepage_list_lock(list);
    minheap_insert(list->hugepage_minheap, &hp->minheap_node, hp->virt_base);
    hugepage_list_unlock(list, iflag);
}

typedef struct minheap_node *(minheap_fn) (struct minheap *);
static struct hugepage *core_list_do_op(struct hugepage_core_list *hcl,
                                        minheap_fn op) {
    bool iflag = hugepage_list_lock(hcl);

    struct minheap_node *nd = op(hcl->hugepage_minheap);
    if (!nd) {
        hugepage_list_unlock(hcl, iflag);
        return NULL;
    }
    hugepage_list_unlock(hcl, iflag);

    return hugepage_from_minheap_node(nd);
}

struct hugepage *hugepage_core_list_peek(struct hugepage_core_list *hcl) {
    return core_list_do_op(hcl, minheap_peek);
}

struct hugepage *hugepage_core_list_pop(struct hugepage_core_list *hcl) {
    return core_list_do_op(hcl, minheap_pop);
}

void hugepage_core_list_remove_hugepage(struct hugepage_core_list *hcl,
                                        struct hugepage *hp) {
    bool iflag = hugepage_list_lock(hcl);
    minheap_remove(hcl->hugepage_minheap, &hp->minheap_node);
    hugepage_list_unlock(hcl, iflag);
}
