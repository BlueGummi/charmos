#include <enforce.h>
#include <mem/anon_vma.h>
#include <mem/avc.h>
#include <mem/folio.h>
#include <mem/rmap.h>
#include <mem/vma_range.h>

static inline pgoff_t va_to_index(const struct vma_range *vr, vaddr_t va) {
    return vr->pgoff + ((va - vma_range_start(vr)) >> PAGE_4K_SHIFT);
}

/* Walk anon_vma itree, anon_vma_clone() puts a child range's AVC into all
 * ancestor anon_vma's, so that one tree names every range across fork
 * hierarchy that can reach this folio's pages */
void rmap_walk_anon(struct folio *f, rmap_visit_fn visit, void *private) {
    struct anon_vma *anon_vma = folio_get_anon_vma(f);
    pgoff_t first = f->index;
    pgoff_t last = first + folio_nr_pages(f) - 1;

    anon_vma_read_lock(anon_vma);

    struct anon_vma_chain *avc;
    for (avc = anon_vma_itree_first(anon_vma, first, last); avc;
         avc = anon_vma_itree_next(avc, first, last)) {
        struct vma_range *vr = avc->vma_range;
        vaddr_t va = vma_range_address(vr, f->index);

        /* vma_range_address gives folio's base index, for multi-page folio
         * only partially covered by this range, this can fall outside it,
         * so we skip them */
        if (va < vma_range_start(vr) || va >= vma_range_end(vr))
            continue;

        visit(vr->mm, va, f, private);
    }

    anon_vma_unlock(anon_vma);
}

/*
 * TODO: folio teardown must anon_vma_putref(folio_get_anon_vma(f)) to balance
 * this getref once folio_free() handles anon folios */

/* First PTE for a new anon folio, now becoming anonymous, owned by the faulting
 * range's anon_vma, gaining first mapper */
void folio_add_anon_rmap_new(struct folio *f, struct vma_range *vr,
                             vaddr_t va) {
    enforce(vr->anon_vma);

    /* TODO: we might consider returning errors for this because
     * the refcount getref might fail? */
    enforce(anon_vma_getref(vr->anon_vma));
    folio_set_anon(f, vr->anon_vma, va_to_index(vr, va));
    folio_set_flag(f, FOLIO_FLAG_MAPPED);
    folio_mapcount_inc(f);
}

/* New PTE maps already anon folio at `va`, which is already anon */
void folio_add_anon_rmap(struct folio *f, struct vma_range *vr, vaddr_t va) {
    enforce(folio_is_anon(f));

    pgoff_t idx = va_to_index(vr, va);
    enforce(idx >= f->index && idx < f->index + folio_nr_pages(f));

    folio_mapcount_inc(f);
}

/* e.g. fork: parent and child keep the PTE, folio gains a mapper */
void folio_add_anon_rmap_shared(struct folio *f, struct vma_range *vr) {
    cc_ignore(vr);
    enforce(folio_is_anon(f));

    folio_mapcount_inc(f);
}

void folio_remove_rmap(struct folio *f, struct vma_range *vr) {
    cc_ignore(vr);
    enforce(folio_is_anon(f));

    folio_mapcount_dec(f); /* true at 0 -> folio is no longer mapped anywhere */
}
