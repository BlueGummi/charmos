/* @title: Memory Descriptor */
#pragma once
#include <compiler/wrapper.h>
#include <mem/vma_range.h>
#include <structures/rbit.h>
#include <sync/rwlock.h>
#include <types/refcount.h>

enum mm_map_flags {
    MM_MAP_FIXED = 1 << 0,
    MM_MAP_ANON = 1 << 1,
    /* TODO: MM_MAP_SHARED / file-backed... */
};

enum mm_fault_flags {
    MM_FAULT_WRITE = 1 << 0, /* error_code & 0x02 */
    MM_FAULT_USER = 1 << 1,  /* error_code & 0x04 */
    MM_FAULT_INSN = 1 << 2,  /* error_code & 0x10 */
};

enum mm_fault_result {
    MM_FAULT_OK,
    MM_FAULT_RETRY,
    MM_FAULT_SIGSEGV,
    MM_FAULT_OOM
};

/* TODO: hold 6 per-cpu, switch between them with
 * generation counters. we can look towards linux
 * once we go and implement this thing for ASID */
struct mm_context {
    size_t ctx_id;
    atomic_size_t tlb_gen;
    struct cpu_mask cpus;
};

struct mm {
    struct vas *vas;
    struct rbit
        vma_range_tree;  /* VMAs keyed by [start, end-1], augmented `max` */
    struct rwlock lock;  /* top of the lock order */
    paddr_t pml4;        /* physical root of the page tables */
    vaddr_t mmap_cursor; /* next-fit hint: where the last gap search ended */
    refcount_t users;    /* threads sharing this address space */
    refcount_t refcount; /* keep the struct alive past last user */
};

struct mm *mm_alloc(void) cw_alloc();
void mm_free(struct mm *mm);
struct mm *mm_fork(struct mm *src); /* clone VMAs + anon_vma_fork each */

void mm_exit(struct mm *mm); /* exit_mmap: unmap + tear down every VMA,
                              * but leave the struct/pml4 standing */

void mm_vma_range_insert(struct mm *mm,
                         struct vma_range *vma_range); /* assert no overlap */
void mm_vma_range_remove(struct mm *mm, struct vma_range *vma_range);
struct vma_range *
mm_vma_range_find(struct mm *mm,
                  vaddr_t addr); /* first vma_range, end > addr */
struct vma_range *mm_vma_range_find_intersection(struct mm *mm, vaddr_t s,
                                                 vaddr_t e);

/* lowest gap >= len in [low, high) */
vaddr_t mm_vma_range_find_gap(struct mm *mm, size_t len, size_t align,
                              vaddr_t low, vaddr_t high);

/* find a gap (or honor a hint), build a vma_range, insert */
vaddr_t mm_map(struct mm *mm, vaddr_t hint, size_t len,
               enum vma_range_protection prot, enum mm_map_flags flags);

/* tear down [start, start+len): split VMAs at the boundaries, unmap their
 * PTEs, drop rmap, free/trim the VMAs */
enum err mm_unmap(struct mm *mm, vaddr_t start, size_t len);

/* change protection over a range: split at boundaries, vma_range_set_prot each,
 * re-protect the live PTEs */
enum err mm_protect(struct mm *mm, vaddr_t start, size_t len,
                    enum vma_range_protection prot);

enum err mm_pgtable_init(struct mm *mm); /* vmm_make_user_pml4() -> mm->pml4 */

void mm_pgtable_free(struct mm *mm); /* free user PT pages; never the
                                      * shared kernel higher-half */

void mm_activate(struct mm *mm); /* load CR3 = mm->pml4 on switch */

enum err mm_map_page(struct mm *mm, vaddr_t va, paddr_t pa, uint64_t pflags);
void mm_unmap_page(struct mm *mm, vaddr_t va);
paddr_t mm_query(struct mm *mm, vaddr_t va, uint64_t *pflags_out);

enum mm_fault_result mm_fault(struct mm *mm, vaddr_t addr,
                              enum mm_fault_flags flags);

enum mm_fault_result mm_do_anon_fault(struct vma_range *vma_range, vaddr_t addr,
                                      enum mm_fault_flags);
enum mm_fault_result mm_do_cow_fault(struct vma_range *vma_range, vaddr_t addr,
                                     enum mm_fault_flags);

static inline bool mm_getref(struct mm *mm) {
    return refcount_inc_not_zero(&mm->refcount);
}

static inline void mm_putref(struct mm *mm) {
    if (refcount_dec_and_test(&mm->refcount))
        mm_free(mm);
}

/* a new thread joins the address space */
static inline bool mm_users_getref(struct mm *mm) {
    return refcount_inc_not_zero(&mm->users);
}

/* last user -> mm_exit(mm); then mm_putref() */
static inline void mm_users_putref(struct mm *mm) {
    if (refcount_dec_and_test(&mm->users)) {
        mm_exit(mm);
        mm_putref(mm); /* release the mm_refcount that mm_users held */
    }
}
