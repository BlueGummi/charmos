/* @title: Page Table */
#pragma once
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

#define PTE_LOCK_BIT ((uint64_t) 1 << 9)
#define PTE_DEAD_BIT ((uint64_t) 1 << 10)
#define PTE_AVAIL2_BIT ((uint64_t) 1 << 11)

typedef _Atomic uint64_t pte_atomic_t;

static inline bool pte_trylock_bit(pte_atomic_t *pte) {
    uint64_t old = atomic_load_explicit(pte, memory_order_relaxed);
    do {
        /* If already locked, or the child table is dead, fail immediately. */
        if (old & (PTE_LOCK_BIT | PTE_DEAD_BIT))
            return false;
    } while (!atomic_compare_exchange_weak_explicit(
        pte, &old, old | PTE_LOCK_BIT,
        memory_order_acquire, /* success: pairs with unlock release */
        memory_order_relaxed)); /* failure: no ordering needed */
    return true;
}

static inline void pte_unlock_bit_internal(pte_atomic_t *pte) {
    atomic_fetch_and_explicit(pte, ~PTE_LOCK_BIT, memory_order_release);
}

static inline void pte_mark_dead(pte_atomic_t *pte) {
    /* We hold the lock, so this is safe as a plain fetch_or. */
    atomic_fetch_or_explicit(pte, PTE_DEAD_BIT, memory_order_release);
}

enum pte_lock_result {
    PTE_LOCK_OK = 0,
    PTE_LOCK_NOT_PRESENT, /* PTE is not present; no child table to lock  */
    PTE_LOCK_DEAD, /* Child table is being freed; caller must retry or abort */
};

static inline enum pte_lock_result pte_lock_internal(pte_atomic_t *pte) {
    for (;;) {
        uint64_t old = atomic_load_explicit(pte, memory_order_relaxed);

        if (!(old & PAGE_PRESENT))
            return PTE_LOCK_NOT_PRESENT;

        if (old & PTE_DEAD_BIT) {
            kassert("impossible");
            return PTE_LOCK_DEAD;
        }

        if (old & PTE_LOCK_BIT) {
            cpu_relax();
            continue;
        }

        if (atomic_compare_exchange_weak_explicit(pte, &old, old | PTE_LOCK_BIT,
                                                  memory_order_acquire,
                                                  memory_order_relaxed))
            return PTE_LOCK_OK;

        /* CAS failed due to a benign race; retry. */
        cpu_relax();
    }
}

static inline enum irql pte_lock_irql(pte_atomic_t *pte,
                                      enum pte_lock_result *result_out) {
    enum irql old_irql = irql_raise(IRQL_DISPATCH_LEVEL);
    enum pte_lock_result r = pte_lock_internal(pte);

    if (r != PTE_LOCK_OK) {
        irql_lower(old_irql);
        *result_out = r;
        return IRQL_NONE;
    }

    *result_out = PTE_LOCK_OK;
    return old_irql;
}

static inline void pte_unlock_irql(pte_atomic_t *pte, enum irql old_irql) {
    pte_unlock_bit_internal(pte);
    irql_lower(old_irql);
}
