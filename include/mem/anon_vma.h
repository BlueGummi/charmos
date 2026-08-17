/* @title: Anonymous Virtual Memory Area */
#pragma once
#include <errno.h>
#include <structures/rbit.h>
#include <sync/rwlock.h>
#include <types/refcount.h>
#include <types/types.h>

/* LOCK ORDERING:
 *
 * mmap_lock
 * └─ anon_vma->root->lock
 *    └─ folio_lock
 */

struct vma_range;

struct anon_vma {
    struct rbit tree; /* AVCs, keyed in OBJECT-SPACE [pgoff, pgoff+npages) */
    struct anon_vma *root;   /* oldest ancestor, the lock is here */
    struct anon_vma *parent; /* immediate ancestor in fork hierarchy */
    struct rwlock lock;      /* acquire via root */
    refcount_t refcount;     /* # AVCs + folios referencing this object */
};

struct anon_vma *anon_vma_alloc(void);
void anon_vma_free(struct anon_vma *av); /* refcount==0 only */
enum errno anon_vma_fork(struct vma_range *child, struct vma_range *parent);

/* root->lock */
static inline void anon_vma_read_lock(struct anon_vma *av) {
    rw_read_lock(&av->root->lock);
}

static inline void anon_vma_unlock(struct anon_vma *av) {
    rw_unlock(&av->root->lock);
}

static inline void anon_vma_write_lock(struct anon_vma *av) {
    rw_write_lock(&av->root->lock);
}

enum errno anon_vma_clone(struct vma_range *dst, struct vma_range *src);

struct anon_vma_chain *anon_vma_itree_first(struct anon_vma *av, pgoff_t first,
                                            pgoff_t last);
struct anon_vma_chain *anon_vma_itree_next(struct anon_vma_chain *avc,
                                           pgoff_t first, pgoff_t last);

/* teardown: unlink all AVCs on this vma_range, drop anon_vma refs */
void vma_range_unlink_anon_vmas(struct vma_range *vma_range);

static inline bool anon_vma_is_root(const struct anon_vma *av) {
    return av->root == av;
}

static inline bool anon_vma_get(struct anon_vma *av) {
    return refcount_inc_not_zero(&av->refcount);
}

/* Drop ref: When forked anon_vma dies it releases its root pin, allowing
 * the put to go up the hierarchy, but it's bounded, as root is its own root */
static inline void anon_vm_area_put(struct anon_vma *av) {
    while (av) {
        struct anon_vma *root = av->root;
        if (!refcount_dec_and_test(&av->refcount))
            return;
        anon_vma_free(av);
        av = (root != av) ? root : NULL;
    }
}
