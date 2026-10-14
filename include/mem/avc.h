/* @title: Anonymous VMA Chain */
#pragma once
#include <compiler/wrapper.h>
#include <mem/page.h>
#include <mem/vma_range.h>
#include <structures/list.h>
#include <structures/rbit.h>
#include <types/types.h>

struct anon_vma_chain {
    struct vma_range *vma_range;
    struct anon_vma *anon_vma;
    struct list_node same_vma_range; /* on vma_range->anon_vma_chain */
    struct rbit_node itnode;         /* in anon_vma->tree      */
};

struct anon_vma_chain *avc_alloc(void) cw_alloc();
void avc_free(struct anon_vma_chain *avc);

/* Link a vma_range to an anon_vma: push onto vma_range's list
 * AND insert into av's tree */
void avc_link(struct vma_range *vma_range, struct anon_vma *av,
              struct anon_vma_chain *avc);
void avc_unlink(struct anon_vma_chain *avc);

/* re-key AVC after vma_range page range changed,
 * reinsert tree node afterwards */
void avc_rekey(struct anon_vma_chain *avc);

/* what the tree is keyed on */
static inline pgoff_t avc_first(const struct anon_vma_chain *a) {
    return a->vma_range->pgoff;
}

static inline pgoff_t avc_last(const struct anon_vma_chain *a) {
    return a->vma_range->pgoff + vma_range_pages(a->vma_range) - 1;
}
