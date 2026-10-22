#include <mem/anon_vma.h>
#include <mem/avc.h>
#include <mem/fixed_size_alloc.h>
#include <mem/pmm.h>
#include <mem/vma_range.h>
#include <smp/perdomain.h>

FIXED_SIZE_RANGE_PERDOMAIN_DEFINE(
    avc, .obj_size = sizeof(struct anon_vma_chain),
    .obj_align = cc_alignof(struct anon_vma_chain));

struct anon_vma_chain *avc_alloc() {
    return FSR_PERDOMAIN_ALLOC(avc);
}

void avc_free(struct anon_vma_chain *f) {
    return FSR_PERDOMAIN_FREE(avc, f);
}

void avc_link(struct vma_range *vma_range, struct anon_vma *av,
              struct anon_vma_chain *avc) {
    avc->anon_vma = av;
    avc->vma_range = vma_range;
    /* Key the interval-tree node in OBJECT space. rbit_insert() initializes
     * every other field of the node, so the interval is all we owe it. Setting
     * it *after* rbit_init_node() (which zeroes the interval) was the bug. */
    avc->itnode.interval.low = avc_first(avc);
    avc->itnode.interval.high = avc_last(avc);
    anon_vma_write_lock(av);

    list_head_push_back(&vma_range->anon_vma_chain, &avc->same_vma_range);
    rbit_insert(&av->tree, &avc->itnode);
    anon_vma_unlock(av);
}

void avc_unlink(struct anon_vma_chain *avc) {
    struct anon_vma *av = avc->anon_vma;
    anon_vma_write_lock(av);
    list_node_delete(&avc->same_vma_range);
    rbit_delete(&av->tree, &avc->itnode);
    anon_vma_unlock(av);
}

void avc_rekey(struct anon_vma_chain *avc) {
    struct anon_vma *av = avc->anon_vma;
    anon_vma_write_lock(av);
    rbit_delete(&av->tree, &avc->itnode);
    avc->itnode.interval.low = avc_first(avc);
    avc->itnode.interval.high = avc_last(avc);
    rbit_insert(&av->tree, &avc->itnode);
    anon_vma_unlock(av);
}
