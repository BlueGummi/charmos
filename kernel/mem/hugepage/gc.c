#include <kassert.h>
#include <mem/hugepage.h>
#include <sch/defer.h>

#include "internal.h"

void hugepage_gc_add(struct hugepage *hp) {
    enum irql irql = hugepage_gc_list_lock_irq_disable(&hugepage_gc_list);

    list_add(&hp->gc_list_node, &hugepage_gc_list.hugepages_list);
    hugepage_gc_list_inc_count(&hugepage_gc_list);

    hugepage_gc_list_unlock(&hugepage_gc_list, irql);
}

/* No locks are taken here */
void hugepage_gc_remove_internal(struct hugepage *hp) {
    list_del(&hp->gc_list_node);
    hugepage_gc_list_dec_count(&hugepage_gc_list);
}

void hugepage_gc_remove(struct hugepage *hp) {
    enum irql irql = hugepage_gc_list_lock_irq_disable(&hugepage_gc_list);

    hugepage_gc_remove_internal(hp);

    hugepage_gc_list_unlock(&hugepage_gc_list, irql);
}

static void hugepage_reset(struct hugepage *hp) {
    hugepage_unmark_for_deletion(hp);
    hp->allocation_type = 0;
    hp->being_deleted = false;

    hugepage_zero_bitmap(hp);

    hp->for_deletion = false;
    hp->last_allocated_idx = 0;
    hp->pages_used = 0;
}

/* This is used when creating a new hugepage.
 * We can check if the gc list has anything
 * and just take that hugepage */
struct hugepage *hugepage_get_from_gc_list(void) {
    enum irql irql = hugepage_gc_list_lock_irq_disable(&hugepage_gc_list);

    struct list_head *nd = NULL;
    struct hugepage *hp = NULL;

    while (!list_empty(&hugepage_gc_list.hugepages_list)) {
        nd = list_pop_front(&hugepage_gc_list.hugepages_list);
        hp = hugepage_from_gc_list_node(nd);

        if (!hugepage_is_being_deleted(hp)) {
            hugepage_reset(hp);
            break;
        }

        hp = NULL;
    }

    hugepage_gc_list_unlock(&hugepage_gc_list, irql);
    return hp;
}

static inline bool hugepage_should_delete() {
    return hugepage_gc_list.pages_in_list > HUGEPAGE_GC_LIST_MAX_HUGEPAGES;
}

static void hugepage_untrack(struct hugepage *hp) {
    hugepage_tb_remove(hugepage_full_tree->htb, hp);
    hugepage_remove_from_core_list_safe(hp, /* locked = */ false);
    hugepage_tree_remove(hugepage_full_tree, hp);
}

bool hugepage_gc_enqueue(struct hugepage *hp) {
    hugepage_untrack(hp);
    hugepage_deletion_sanity_assert(hp);
    hugepage_mark_for_deletion(hp);
    if (hugepage_should_delete())
        return true;

    hugepage_gc_add(hp);
    return false;
}
