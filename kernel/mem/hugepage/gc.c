#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/defer.h>
#include <types/refcount.h>

/*
 *
 * GC list logic - mark page for GC, add from GC list,
 * remove from GC list
 *
 */

/* The first few hugepages have a longer GC list
 * timeout, whereas the last few have shorter ones */
time_t hugepage_gc_deletion_timeout(void) {
    return (HUGEPAGE_GC_LIST_MAX_HUGEPAGES - hugepage_gc_list.pages_in_list) *
           HUGEPAGE_GC_LIST_TIMEOUT_PER_PAGE;
}

void hugepage_gc_add(struct hugepage *hp) {
    bool iflag = hugepage_gc_list_lock(&hugepage_gc_list);

    list_add(&hp->gc_list_node, &hugepage_gc_list.hugepages_list);
    atomic_fetch_add(&hugepage_gc_list.pages_in_list, 1);

    hugepage_gc_list_unlock(&hugepage_gc_list, iflag);
}

void hugepage_gc_remove(struct hugepage *hp) {
    bool iflag = hugepage_gc_list_lock(&hugepage_gc_list);

    list_del(&hp->gc_list_node);
    atomic_fetch_sub(&hugepage_gc_list.pages_in_list, 1);

    hugepage_gc_list_unlock(&hugepage_gc_list, iflag);
}

/* This is used when creating a new hugepage.
 * We can check if the gc list has anything
 * and just take that hugepage */
struct hugepage *hugepage_get_from_gc_list(void) {
    bool iflag = hugepage_gc_list_lock(&hugepage_gc_list);

    struct list_head *nd = list_pop_front(&hugepage_gc_list.hugepages_list);
    atomic_fetch_sub(&hugepage_gc_list.pages_in_list, 1);

    hugepage_gc_list_unlock(&hugepage_gc_list, iflag);

    struct hugepage *hp = hugepage_from_gc_list_node(nd);
    if (!atomic_load(&hp->being_deleted) && hugepage_trylock(hp)) {
        hp->for_deletion = false;

        /* Sentinel value */
        hp->deletion_timeout = HUGEPAGE_DELETION_TIMEOUT_NONE;
        return hp;
    }
    return NULL;
}

static inline void hugepage_mark_for_deletion(struct hugepage *hp) {
    hp->deletion_timeout = hugepage_gc_deletion_timeout();
    hp->for_deletion = true;
}

static void hugepage_delete_dpc(void *arg1, void *arg2) {
    (void) arg2;
    struct hugepage *hp = arg1;
    hugepage_delete(hp);
}

void hugepage_enqueue_for_gc(struct hugepage *hp) {
    if (hp->minheap_node.index != MINHEAP_INDEX_INVALID) {
        struct hugepage_core_list *hcl = hugepage_get_core_list(hp);
        hugepage_core_list_remove_hugepage(hcl, hp);
    }

    hugepage_tree_remove(hugepage_full_tree, hp);
    hugepage_deletion_sanity_assert(hp);

    hugepage_mark_for_deletion(hp);

    /* Immediately delete */
    if (hp->deletion_timeout == 0) {
        hugepage_delete(hp);
        return;
    }

    /* Only enqueue if this has a timeout */
    hugepage_gc_add(hp);
    if (atomic_exchange(&hp->gc_timer_pending, true) == false) {
        defer_enqueue(hugepage_delete_dpc, hp, NULL, hp->deletion_timeout);
    }
}
