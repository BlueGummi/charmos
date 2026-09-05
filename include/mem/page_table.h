/* @title: Page Table */
#pragma once
#include <asm.h>
#include <irq/irq.h>
#include <mem/page.h>
#include <sch/irql.h>
#include <stdatomic.h>
#include <stdint.h>

#define PT_LEVELS 4

#define PT_SHIFT_L1 12
#define PT_SHIFT_L2 21
#define PT_SHIFT_L3 30
#define PT_SHIFT_L4 39

#define PT_STRIDE 9

#define PTE_LOCK_SHIFT 9
#define PTE_AVAIL2_SHIFT 10
#define PTE_LOCK_BIT ((uint64_t) 1 << PTE_LOCK_SHIFT)
#define PTE_AVAIL2_BIT ((uint64_t) 1 << PTE_AVAIL2_SHIFT)

/* Mark an entry whose child table is shared with other entries, this is what
 * we use in aliasing. The subtree should never be freed through this entry,
 * since every other entry pointing at it would see the change too. This
 * should only ever be set on present entries */
#define PTE_SHARED_SHIFT 11
#define PTE_SHARED_BIT ((uint64_t) 1 << PTE_SHARED_SHIFT)

static inline bool pte_is_shared(uint64_t pte) {
    return (pte & PAGE_PRESENT) && (pte & PTE_SHARED_BIT);
}

/* Packed layout:
 *
 *   bit  0       P = 0
 *   bits 1-2     type
 *   bits 3-8     payload low  (6 bits)
 *   bit  9       LOCK    (not payload)
 *   bit  10      AVAIL2  (not payload)
 *   bits 11-63   payload high (53 bits)
 *
 *   Do note that SHARED is not accounted for here, because
 *   that bit is only ever set on PRESENT entries
 */
#define PTE_TAGGED_TYPE_SHIFT (PAGE_PRESENT_SHIFT + 1)
#define PTE_TAGGED_TYPE_MASK 0x3ULL

#define PTE_TAGGED_PAYLOAD_LOW_BITS 6
#define PTE_TAGGED_PAYLOAD_LOW_SHIFT 3 /* packed position of the low chunk */
#define PTE_TAGGED_PAYLOAD_HIGH_SHIFT                                          \
    11 /* packed position of the high chunk                                    \
        */
#define PTE_TAGGED_PAYLOAD_LOW_MASK ((1ULL << PTE_TAGGED_PAYLOAD_LOW_BITS) - 1)
#define PTE_TAGGED_PAYLOAD_BITS                                                \
    (PTE_TAGGED_PAYLOAD_LOW_BITS + (64 - PTE_TAGGED_PAYLOAD_HIGH_SHIFT))

#define PTE_TAGGED_GET_TYPE(pte)                                               \
    (((pte) >> PTE_TAGGED_TYPE_SHIFT) & PTE_TAGGED_TYPE_MASK)
#define PTE_TAGGED_SET_TYPE(type) ((uint64_t) (type) << PTE_TAGGED_TYPE_SHIFT)

typedef _Atomic uint64_t pte_atomic_t;

/* Packed into the low X bits */
enum pte_tag_type : uint8_t {
    PTE_TAG_TYPE_NONE = 0,
    PTE_TAG_TYPE_DEMAND_PAGED = 1,
};

/* This is a bit like a tagged pointer, just
 * that the payload moseys around LOCK_BIT and AVAIL2,
 * and the type sits right above PRESENT
 *
 * The layout is effectively
 *
 * [ payload higher ] [ LOCK + AVAIL2 ] [ payload lower ] [ type ] [ P = 0 ]
 */
struct pte_tagged {
    enum pte_tag_type type;
    uint64_t payload; /* NOTE: higher 5 bits MUST be zero */
};

static inline void pte_tagged_check(struct pte_tagged *pt) {
    kassert(pt->type != PTE_TAG_TYPE_NONE);
    kassert((pt->payload >> PTE_TAGGED_PAYLOAD_BITS) == 0);
}

static inline uint64_t pte_tagged_pack(struct pte_tagged *pt) {
    /* We leave LOCK + AVAIL2 zeroed here, whoever uses this must OR it into a
     * PTE that already holds those bits, probably atomically */
    uint64_t low = pt->payload & PTE_TAGGED_PAYLOAD_LOW_MASK;
    uint64_t high = pt->payload >> PTE_TAGGED_PAYLOAD_LOW_BITS;

    return PTE_TAGGED_SET_TYPE(pt->type) |
           (low << PTE_TAGGED_PAYLOAD_LOW_SHIFT) |
           (high << PTE_TAGGED_PAYLOAD_HIGH_SHIFT);
}

static inline struct pte_tagged pte_tagged_unpack(pte_t pte) {
    struct pte_tagged pt;
    pt.type = PTE_TAGGED_GET_TYPE(pte);

    uint64_t low =
        (pte >> PTE_TAGGED_PAYLOAD_LOW_SHIFT) & PTE_TAGGED_PAYLOAD_LOW_MASK;
    uint64_t high = pte >> PTE_TAGGED_PAYLOAD_HIGH_SHIFT;

    pt.payload = (high << PTE_TAGGED_PAYLOAD_LOW_BITS) | low;
    return pt;
}

static inline bool pte_locked(pte_atomic_t *pte) {
    return atomic_load_explicit(pte, memory_order_relaxed) & PTE_LOCK_BIT;
}

static inline uint64_t pte_read(pte_atomic_t *pte) {
    return atomic_load_explicit(pte, memory_order_relaxed);
}

static inline uint64_t pte_or(pte_atomic_t *pte, uint64_t val) {
    return atomic_fetch_or_explicit(pte, val, memory_order_acq_rel);
}

static inline void pte_unlock_internal(pte_atomic_t *pte) {
    kassert(
        atomic_fetch_and_explicit(pte, ~PTE_LOCK_BIT, memory_order_release) &
        PTE_LOCK_BIT);
}

/* TODO: use bit_spinlock.h */
static inline void pte_lock_internal(pte_atomic_t *pte) {
    for (;;) {
        uint64_t old = atomic_load_explicit(pte, memory_order_relaxed);

        if (old & PTE_LOCK_BIT) {
            cpu_relax();
            continue;
        }

        if (atomic_compare_exchange_weak_explicit(pte, &old, old | PTE_LOCK_BIT,
                                                  memory_order_acquire,
                                                  memory_order_relaxed))
            return;

        cpu_relax();
    }
}

static inline enum irql pte_lock_irql(pte_atomic_t *pte) {
    enum irql old_irql = irql_raise(IRQL_DISPATCH_LEVEL);
    pte_lock_internal(pte);
    return old_irql;
}

static inline void pte_unlock_irql(pte_atomic_t *pte, enum irql old_irql) {
    pte_unlock_internal(pte);
    irql_lower(old_irql);
}

static inline bool pte_has_tag(pte_atomic_t *pte) {
    kassert(!(pte_read(pte) & PAGE_PRESENT));
    return (pte_read(pte) & ~(PAGE_PRESENT | PTE_LOCK_BIT | PTE_AVAIL2_BIT)) !=
           0;
}

static inline uint64_t pte_tag_pte(pte_atomic_t *pte, struct pte_tagged *ptag) {
    kassert(pte_locked(pte));
    kassert(!pte_has_tag(pte));
    pte_tagged_check(ptag);
    uint64_t packed = pte_tagged_pack(ptag);
    return pte_or(pte, packed);
}

static inline struct pte_tagged pte_read_tag(pte_atomic_t *pte) {
    kassert(pte_has_tag(pte));
    return pte_tagged_unpack(pte_read(pte));
}

static inline bool pte_in_use(pte_atomic_t *pte) {
    if (pte_read(pte) & PAGE_PRESENT)
        return true;

    return pte_has_tag(pte);
}
