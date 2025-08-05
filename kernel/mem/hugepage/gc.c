#include <kassert.h>
#include <mem/hugepage.h>
#include <sch/defer.h>

void hugepage_gc_add(struct hugepage *hp) {
    bool iflag = hugepage_gc_list_lock(&hugepage_gc_list);

    list_add(&hp->gc_list_node, &hugepage_gc_list.hugepages_list);
    atomic_fetch_add(&hugepage_gc_list.pages_in_list, 1);

    hugepage_gc_list_unlock(&hugepage_gc_list, iflag);
}

void hugepage_gc_remove_internal(struct hugepage *hp) {
    list_del(&hp->gc_list_node);
    atomic_fetch_sub(&hugepage_gc_list.pages_in_list, 1);
}

void hugepage_gc_remove(struct hugepage *hp) {
    bool iflag = hugepage_gc_list_lock(&hugepage_gc_list);

    hugepage_gc_remove_internal(hp);

    hugepage_gc_list_unlock(&hugepage_gc_list, iflag);
}

/* This is used when creating a new hugepage.
 * We can check if the gc list has anything
 * and just take that hugepage */
struct hugepage *hugepage_get_from_gc_list(void) {
    bool iflag = hugepage_gc_list_lock(&hugepage_gc_list);

    struct list_head *nd = NULL;
    struct hugepage *hp = NULL;

    while (!list_empty(&hugepage_gc_list.hugepages_list)) {
        nd = list_pop_front(&hugepage_gc_list.hugepages_list);
        hp = hugepage_from_gc_list_node(nd);

        if (!atomic_load(&hp->being_deleted)) {
            atomic_fetch_sub(&hugepage_gc_list.pages_in_list, 1);
            atomic_store(&hp->for_deletion, false);
            break;
        }

        hp = NULL;
    }

    hugepage_gc_list_unlock(&hugepage_gc_list, iflag);
    return hp;
}

static inline void hugepage_mark_for_deletion(struct hugepage *hp) {
    hp->for_deletion = true;
}

static inline bool hugepage_still_in_core_list(struct hugepage *hp) {
    return hp->minheap_node.index != MINHEAP_INDEX_INVALID;
}

static inline void hugepage_remove_from_core_list(struct hugepage *hp) {
    struct hugepage_core_list *hcl = hugepage_get_core_list(hp);
    hugepage_core_list_remove_hugepage(hcl, hp);
}

static inline bool hugepage_try_instant_delete(struct hugepage *hp) {
    if (hugepage_gc_list.pages_in_list > HUGEPAGE_GC_LIST_MAX_HUGEPAGES) {
        hugepage_delete(hp);
        return true;
    }
    return false;
}

void hugepage_enqueue_for_gc(struct hugepage *hp) {
    if (hugepage_still_in_core_list(hp))
        hugepage_remove_from_core_list(hp);

    hugepage_tree_remove(hugepage_full_tree, hp);
    hugepage_deletion_sanity_assert(hp);

    hugepage_mark_for_deletion(hp);

    if (hugepage_try_instant_delete(hp))
        return;

    /* Only enqueue if this has a timeout */
    hugepage_gc_add(hp);
}
