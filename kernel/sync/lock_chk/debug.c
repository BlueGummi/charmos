#ifdef DEBUG_LOCK_CHK

#include <asm.h>
#include <atomic.h>
#include <kassert.h>
#include <smp/percpu.h>
#include <sync/lock_chk.h>

#include "internal.h"

PERCPU_DEFINE(struct lock_debug_cpu, lock_debug_cpu);

static bool debug_is_active(void) {
    return lock_chk_state_active(&lock_chk_global.debug);
}

static struct lock_debug_cpu *debug_cpu_here(void) {
    kassert(!irqs_enabled());
    return PERCPU_PTR(TOPC_IFLAG, lock_debug_cpu);
}

void lock_debug_activate(void) {
    kassert(PERCPU_READY(lock_debug_cpu));
    atomic_store_release(&lock_chk_global.debug, LOCK_CHK_ACTIVE);
}

void lock_debug_spin_classify(atomic(enum lock_op_flags) * usage,
                              enum lock_op_flags requested,
                              struct lock_chk_lock *lock,
                              const struct lock_chk_site *site) {
    if (!debug_is_active())
        return;

    uint8_t expected = LOCK_OP_IRQ_NONE;
    if (atomic_cas_strong(usage, &expected, requested, mo_rlx, mo_rlx))
        return;

    if ((expected & LOCK_OP_IRQ_MASK) != (requested & LOCK_OP_IRQ_MASK)) {
        struct lock_chk_fault fail = lock_chk_fault_from_lock(
            LOCK_CHK_FAIL_CONTEXT, lock, site, LOCK_CHK_MODE_IGNORED);
        lock_chk_fail(&fail, "%s %p changed IRQL usage",
                      lock_chk_type_to_str(lock->type), lock->instance);
    }
}

bool lock_debug_spin_push(struct lock_chk_lock *lock, enum irql prev_irql,
                          const struct lock_chk_site *site) {
    if (!debug_is_active())
        return false;

    struct lock_debug_cpu *cpu = debug_cpu_here();
    if (cpu->depth == LOCK_CHK_MAX_SPIN_DEPTH) {
        struct lock_chk_fault fail = lock_chk_fault_from_lock(
            LOCK_CHK_FAIL_CAPACITY, lock, site, LOCK_CHK_MODE_IGNORED);
        fail.capacity_pool = "shallow spin stack";
        fail.capacity_used = LOCK_CHK_MAX_SPIN_DEPTH;
        fail.capacity_limit = LOCK_CHK_MAX_SPIN_DEPTH;
        lock_chk_fail(&fail, "Shallow spin stack capacity exhausted (%u/%u)",
                      LOCK_CHK_MAX_SPIN_DEPTH, LOCK_CHK_MAX_SPIN_DEPTH);
        atomic_store_release(&lock_chk_global.debug, LOCK_CHK_DEGRADED);
        return false;
    }

    cpu->stack[cpu->depth++] = (struct lock_debug_spin_entry){
        .instance = lock->instance,
        .acquire_site = site,
        .prev_irql = prev_irql,
        .type = lock->type,
    };
    return true;
}

void lock_debug_spin_validate_top(struct lock_chk_lock *lock,
                                  enum irql prev_irql,
                                  const struct lock_chk_site *site) {
    void *instance = lock->instance;
    enum lock_chk_type type = lock->type;
    if (!debug_is_active())
        return;

    struct lock_debug_cpu *cpu = debug_cpu_here();
    if (cpu->depth == 0) {
        struct lock_chk_fault fail = lock_chk_fault_from_lock(
            LOCK_CHK_FAIL_SPIN_ORDER, lock, site, LOCK_CHK_MODE_IGNORED);
        lock_chk_fail(&fail, "Releasing untracked %s %p",
                      lock_chk_type_to_str(type), instance);
        return;
    }

    struct lock_debug_spin_entry *top = &cpu->stack[cpu->depth - 1];
    if (top->instance != instance || top->type != type ||
        top->prev_irql != prev_irql) {
        struct lock_chk_fault fail = lock_chk_fault_from_lock(
            LOCK_CHK_FAIL_SPIN_ORDER, lock, site, LOCK_CHK_MODE_IGNORED);
        lock_chk_fail(&fail, "Non-LIFO %s release %p",
                      lock_chk_type_to_str(type), instance);
    }
}

void lock_debug_spin_pop(struct lock_chk_lock *lock) {
    if (!debug_is_active())
        return;

    struct lock_debug_cpu *cpu = debug_cpu_here();
    kassert(cpu->depth != 0);
    struct lock_debug_spin_entry *top = &cpu->stack[cpu->depth - 1];
    kassert(top->instance == lock->instance);
    kassert(top->type == lock->type);
    cpu->depth--;
}

#endif /* DEBUG_LOCK_CHK */
