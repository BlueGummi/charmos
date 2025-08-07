#include <kassert.h>
#include <mem/hugepage.h>
#include <sch/defer.h>

void hugepage_gc_add(struct hugepage *hp) {
    bool iflag = hugepage_gc_list_lock(&hugepage_gc_list);

    list_add(&hp->gc_list_node, &hugepage_gc_list.hugepages_list);
    hugepage_gc_list_inc_count(&hugepage_gc_list);

    hugepage_gc_list_unlock(&hugepage_gc_list, iflag);
}

void hugepage_gc_remove_internal(struct hugepage *hp) {
    list_del(&hp->gc_list_node);

    hugepage_gc_list_dec_count(&hugepage_gc_list);
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

        if (!hugepage_is_being_deleted(hp)) {
            hugepage_unmark_for_deletion(hp);
            break;
        }

        hp = NULL;
    }

    hugepage_gc_list_unlock(&hugepage_gc_list, iflag);
    return hp;
}

static inline bool hugepage_try_instant_delete(struct hugepage *hp) {
    if (hugepage_gc_list.pages_in_list > HUGEPAGE_GC_LIST_MAX_HUGEPAGES) {
        hugepage_delete(hp);
        return true;
    }
    return false;
}

void hugepage_gc_enqueue(struct hugepage *hp) {
    hugepage_tb_remove(hugepage_full_tree->htb, hp);
    hugepage_remove_from_list_safe(hp);
    hugepage_tree_remove(hugepage_full_tree, hp);

    hugepage_deletion_sanity_assert(hp);

    hugepage_mark_for_deletion(hp);
    if (hugepage_try_instant_delete(hp))
        return;

    /* Only enqueue if this has a timeout */
    hugepage_gc_add(hp);
}
