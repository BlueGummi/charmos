#include <global.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <mem/fixed_size_alloc.h>
#include <mem/hhdm.h>
#include <mem/pmm.h>
#include <string.h>

static inline size_t fixed_size_header_size(struct fixed_size_range *fsr) {
    return ALIGN_UP(sizeof(struct fixed_size_page_hdr), fsr->attrs.obj_align);
}

static inline size_t fixed_size_per_page(struct fixed_size_range *fsr) {
    return (PAGE_SIZE - fixed_size_header_size(fsr)) / fsr->full_node_size;
}

static inline struct fixed_size_node *fsn_for_obj(struct fixed_size_range *fsr,
                                                  void *obj) {
    return (struct fixed_size_node *) (obj + fsr->full_node_size -
                                       sizeof(struct fixed_size_node));
}

static inline void *obj_for_fsn(struct fixed_size_range *fsr,
                                struct fixed_size_node *fsn) {
    return (void *) ((uintptr_t) fsn -
                     (fsr->full_node_size - sizeof(struct fixed_size_node)));
}

static inline void *fixed_size_obj_n(struct fixed_size_range *fsr,
                                     struct fixed_size_page_hdr *hdr,
                                     uint32_t n) {
    uint8_t *objs = (uint8_t *) ((uintptr_t) hdr + fixed_size_header_size(fsr));
    return (objs + fsr->full_node_size * n);
}

static void fixed_size_drop_page(struct fixed_size_range *fsr,
                                 struct fixed_size_page_hdr *hdr) {
    SPINLOCK_ASSERT_HELD(&fsr->lock);
    for (uint32_t i = 0; i < hdr->total; i++) {
        void *obj = fixed_size_obj_n(fsr, hdr, i);
        struct fixed_size_node *fsn = fsn_for_obj(fsr, obj);

        if (fsr->attrs.deinit_obj)
            fsr->attrs.deinit_obj(obj);

        list_del_init(&fsn->list_node);
    }

    list_del(&hdr->page_list);
    fsr->empty_pages--;
    pmm_free_page(hhdm_vaddr_to_paddr((uintptr_t) hdr));
}

static bool fixed_size_refill(struct fixed_size_range *fsr) {
    SPINLOCK_ASSERT_HELD(&fsr->lock);
    uintptr_t phys = pmm_alloc_page();
    if (!phys)
        return false;

    uintptr_t virt = hhdm_paddr_to_vaddr(phys);

    struct fixed_size_page_hdr *hdr = (struct fixed_size_page_hdr *) virt;
    hdr->total = fixed_size_per_page(fsr);
    hdr->free_count = fixed_size_per_page(fsr);
    INIT_LIST_HEAD(&hdr->page_list);
    list_add_tail(&hdr->page_list, &fsr->fl_pages);
    hdr->domain = fsr->domain;
    fsr->empty_pages++;

    for (uint32_t i = 0; i < hdr->total; i++) {
        void *obj = fixed_size_obj_n(fsr, hdr, i);
        struct fixed_size_node *fsn = fsn_for_obj(fsr, obj);
        INIT_LIST_HEAD(&fsn->list_node);
        list_add_tail(&fsn->list_node, &fsr->freelist);

        if (fsr->attrs.init_obj) {
            memset(obj, 0, fsr->attrs.obj_size);
            fsr->attrs.init_obj(obj);
        }
    }

    return true;
}

void *fixed_size_alloc(struct fixed_size_range *fsr) {
    enum irql irql = spin_lock(&fsr->lock);
    if (list_empty(&fsr->freelist)) {
        if (!fixed_size_refill(fsr)) {
            spin_unlock(&fsr->lock, irql);
            return NULL;
        }
    }

    struct list_head *pop = list_pop_front_init(&fsr->freelist);
    struct fixed_size_node *fsn = (struct fixed_size_node *) pop;
    void *obj = obj_for_fsn(fsr, fsn);

    struct fixed_size_page_hdr *hdr = fixed_size_page_of(obj);

    if (hdr->free_count == hdr->total)
        fsr->empty_pages--;

    hdr->free_count--;

    spin_unlock(&fsr->lock, irql);
    return obj;
}

static void fixed_size_free_internal(struct fixed_size_range *fsr, void *obj) {
    enum irql irql = spin_lock(&fsr->lock);
    struct fixed_size_page_hdr *hdr = fixed_size_page_of(obj);
    struct fixed_size_node *fsn = fsn_for_obj(fsr, obj);

    hdr->free_count++;
    list_add_tail(&fsn->list_node, &fsr->freelist);

    if (hdr->free_count != hdr->total) {
        spin_unlock(&fsr->lock, irql);
        return;
    }

    fsr->empty_pages++;
    if (fsr->empty_pages > FIXED_SIZE_KEEP_EMPTY_PAGES)
        fixed_size_drop_page(fsr, hdr);

    spin_unlock(&fsr->lock, irql);
}

/* Wrapper around internal for domain FSRs, finds the right fsr */
void fixed_size_free(struct fixed_size_range *fsr, void *obj) {
    struct fixed_size_page_hdr *hdr = fixed_size_page_of(obj);
    domain_id_t domain = hdr->domain;
    if (!fsr->perdomain_fsrs) {
        kassert(domain == DOMAIN_ID_NONE);

        /* Just one */
        return fixed_size_free_internal(fsr, obj);
    }

    kassert(fsr->perdomain_fsrs[domain]->domain == domain);
    return fixed_size_free_internal(fsr->perdomain_fsrs[domain], obj);
}

void fixed_size_reclaim_freelist_pages(struct fixed_size_range *fsr) {
    struct list_head *pos, *tmp;

    enum irql irql = spin_lock(&fsr->lock);
    list_for_each_safe(pos, tmp, &fsr->fl_pages) {
        struct fixed_size_page_hdr *hdr =
            container_of(pos, struct fixed_size_page_hdr, page_list);

        if (hdr->free_count < hdr->total)
            continue;

        fixed_size_drop_page(fsr, hdr);
    }

    spin_unlock(&fsr->lock, irql);
}

void fixed_size_range_init(struct fixed_size_range *fsr,
                           struct fixed_size_range_attributes *attrs) {
    kassert(attrs->obj_size && attrs->obj_align, "Fill the fields out");
    spinlock_init(&fsr->lock);
    fsr->attrs = *attrs;
    INIT_LIST_HEAD(&fsr->freelist);
    INIT_LIST_HEAD(&fsr->fl_pages);
    fsr->empty_pages = 0;
    fsr->full_node_size = ALIGN_UP(
        sizeof(struct fixed_size_node) + attrs->obj_size, attrs->obj_align);

    /* These are instantiated after init in the macro for perdomain */
    fsr->perdomain_fsrs = NULL;
    fsr->domain = DOMAIN_ID_NONE;
}

struct fixed_size_range *
fixed_size_range_create(struct fixed_size_range_attributes *attrs) {
    struct fixed_size_range *fsr;
    kassert(attrs);
    if (attrs->bootstrap_mode) {
        paddr_t phys = alloc_or_die(pmm_alloc_page());
        vaddr_t virt = hhdm_paddr_to_vaddr(phys);
        fsr = (void *) virt;
    } else {
        fsr = kmalloc(sizeof(struct fixed_size_range));
        if (!fsr)
            return NULL;
    }

    fixed_size_range_init(fsr, attrs);
    return fsr;
}
