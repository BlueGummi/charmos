#include <mem/anon_vma.h>
#include <mem/avc.h>
#include <mem/fixed_size_alloc.h>
#include <mem/vma_range.h>
#include <smp/perdomain.h>
#include <structures/list.h>
#include <types/refcount.h>

FIXED_SIZE_RANGE_PERDOMAIN_DECLARE(anon_vma,
                                   .obj_size = sizeof(struct anon_vma),
                                   .obj_align = _Alignof(struct anon_vma));

struct anon_vma *anon_vma_alloc() {
    struct anon_vma *av = FSR_PERDOMAIN_ALLOC(anon_vma);
    if (!av)
        return NULL;

    rbit_init(&av->tree);

    /* Faults touch anon_vmas from every thread priority, so we gotta
     * set the PI ceiling to URGENT... NOTE: if PI design narrows
     * down who can fault, we can lower this. Best not to spam URGENTs */
    rwlock_init(&av->lock, THREAD_PRIO_CLASS_URGENT);

    /* New anon_vma is its own root and base of fork hierarchy */
    av->root = av;
    av->parent = NULL;

    /* No AVC or folio references, first link bumps */
    refcount_init(&av->refcount, 0);
    return av;
}

void anon_vma_free(struct anon_vma *f) {
    return FSR_PERDOMAIN_FREE(anon_vma, f);
}

/* The big tree iteration
 *
 * Return in object/pgoff order every AVC whose VMA covers any page in the
 * [first, last] interval
 *
 * Augmented interval tree search (classically from CLRS), every rbit_node
 * carries `max`, letting us prune subtrees that cannot overlap
 *
 * NOTE: these don't take anon_vma's lock. it's the caller's job to hold
 * the read lock across the entire first()..next()..NULL walk */
static struct rbit_node *itree_subtree_search(struct rbit_node *node,
                                              pgoff_t first, pgoff_t last) {
    /* invariant: first <= node->max, so an overlap exists below */
    while (true) {
        if (node->left && first <= node->left->max) {
            node = node->left; /* leftmost lives in the left subtree */
            continue;
        }

        if (node->interval.low <= last) { /* node starts at/under the window */

            if (first <= node->interval.high) /* ...ends at or over it -> hit */
                return node;

            if (node->right && first <= node->right->max) {
                node = node->right;
                continue;
            }
        }

        return NULL;
    }
}

struct anon_vma_chain *anon_vma_itree_first(struct anon_vma *av, pgoff_t first,
                                            pgoff_t last) {
    struct rbit_node *root = av->tree.root;
    if (!root || root->max < first)
        return NULL;

    struct rbit_node *n = itree_subtree_search(root, first, last);
    return n ? rbit_entry(n, struct anon_vma_chain, itnode) : NULL;
}

struct anon_vma_chain *anon_vma_itree_next(struct anon_vma_chain *avc,
                                           pgoff_t first, pgoff_t last) {
    struct rbit_node *node = &avc->itnode;
    struct rbit_node *rb = node->right;
    struct rbit_node *prev;

    while (true) {
        /* invariant: node->interval.low <= last; rb == node->right */

        /* the next overlap in-order is either in the right subtree... */
        if (rb && first <= rb->max) {
            struct rbit_node *n = itree_subtree_search(rb, first, last);
            return n ? rbit_entry(n, struct anon_vma_chain, itnode) : NULL;
        }

        /* ...or up the tree at the first ancestor we reach from its left */
        do {
            prev = node;
            node = node->parent;

            if (!node)
                return NULL;

            rb = node->right;
        } while (prev == rb);

        /* lows are sorted, so once an ancestor starts past
         * `last`, so does everything after it */

        /* the walk is finished */
        if (last < node->interval.low)
            return NULL;

        if (first <= node->interval.high)
            return rbit_entry(node, struct anon_vma_chain, itnode);

        /* else: ancestor's interval misses, fall through and probe its right */
    }
}

/* Here, we attach `dst` to every anon_vma that `src` is linked to,
 * so that a child VMA's rmap can find pages that were faulted before the fork
 *
 * One fresh AVC per source link, each holds a reference on the
 * anon_vma it points at */
enum errno anon_vma_clone(struct vma_range *dst, struct vma_range *src) {
    struct anon_vma_chain *src_avc;
    list_for_each_entry(src_avc, &src->anon_vma_chain, same_vma_range) {
        struct anon_vma *av = src_avc->anon_vma;

        struct anon_vma_chain *avc = avc_alloc();
        if (!avc)
            goto enomem;

        refcount_inc(&av->refcount); /* dst's new AVC pins this */
        avc_link(dst, av, avc);
    }
    return ERR_OK;

enomem:

    /* undo the partial chain so `dst` is left as we found it */
    vma_range_unlink_anon_vmas(dst);
    return ERR_NO_MEM;
}

enum errno anon_vma_fork(struct vma_range *child, struct vma_range *parent) {
    /* inherit parent's anon_vmas so already mapped (about
     * to be COW) pages stay reachable from the child */
    enum errno err = anon_vma_clone(child, parent);
    if (err != ERR_OK)
        return err;

    /* no anon stuff in parent, child is unaffected, stays lazy */
    struct anon_vma *pav = parent->anon_vma;
    if (!pav)
        return ERR_OK;

    /* give child its own anon_vma, where future COWed pages will live,
     * sharing the root lock, hanging off parent in fork hierarchy */
    struct anon_vma *av = anon_vma_alloc();
    if (!av)
        goto enomem;

    struct anon_vma_chain *avc = avc_alloc();
    if (!avc) {
        anon_vma_free(av); /* never linked, refcount 0 */
        goto enomem;
    }

    av->parent = pav;
    av->root = pav->root;
    refcount_inc(&av->root->refcount); /* pin root: the lock lives there */

    child->anon_vma = av;
    refcount_inc(&av->refcount); /* AVC we're about to link */
    avc_link(child, av, avc);
    return ERR_OK;

enomem:
    vma_range_unlink_anon_vmas(child);
    return ERR_NO_MEM;
}

/* Drop every AVC on `vma_range`, releasing all the anon_vma references
 *
 * The VMA is anonymous-clean, any anon_vma whose last link/folio went
 * away is freed (going up to the pinned root eventually) */
void vma_range_unlink_anon_vmas(struct vma_range *vma_range) {
    struct anon_vma_chain *avc, *next;

    list_for_each_entry_safe(avc, next, &vma_range->anon_vma_chain,
                             same_vma_range) {
        struct anon_vma *av = avc->anon_vma;
        avc_unlink(avc);
        avc_free(avc);
        anon_vma_put(av);
    }

    vma_range->anon_vma = NULL;
}
