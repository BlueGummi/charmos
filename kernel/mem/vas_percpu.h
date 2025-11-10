#pragma once
#include <mem/pmm.h>
#include <mem/vaddr_alloc.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <types/types.h>

/*
 * Per-cpu VAS set: partition a global range [base, limit) into N cpu subranges.
 */
struct vas_set {
    vaddr_t base;
    vaddr_t limit;
    size_t per_size; /* bytes per CPU partition */
    unsigned ncpus;
    struct vas_space **spaces; /* array length ncpus; each pointer to a
                                  vas_space created by vas_space_bootstrap() */
    struct spinlock lock;
};

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(vas_set, lock);

/* bootstraps a vas_set for ncpus; must not call kmalloc/kfree */
struct vas_set *vas_set_bootstrap(vaddr_t base, vaddr_t limit, unsigned ncpus);

struct vas_set *vas_set_init(vaddr_t base, vaddr_t limit, unsigned ncpus);

/* alloc/free that work with the set. returns 0 on failure. */
vaddr_t vas_set_alloc(struct vas_set *set, size_t size, size_t align);
void vas_set_free(struct vas_set *set, vaddr_t addr);
