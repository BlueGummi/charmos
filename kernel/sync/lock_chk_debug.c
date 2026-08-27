#ifdef DEBUG_LOCK_CHK

#include <asm.h>
#include <kassert.h>
#include <smp/percpu.h>
#include <stdatomic.h>
#include <sync/lock_chk.h>

#include "lock_chk_internal.h"

struct lock_debug_spin_entry {
    void *instance;
    const struct lock_chk_site *acquire_site;
    enum irql prev_irql;
    enum lock_chk_type type;
};

struct lock_debug_cpu {
    struct lock_debug_spin_entry stack[LOCK_CHK_MAX_SPIN_DEPTH];
    uint8_t depth;
};

static _Atomic enum lock_chk_engine_state lock_debug_state = LOCK_CHK_INACTIVE;
PERCPU_DECLARE(lock_debug_cpu, struct lock_debug_cpu, NULL);

static const char *lock_chk_type_name(enum lock_chk_type type) {
    switch (type) {
    case LOCK_CHK_TYPE_SPIN: return "spinlock";
    case LOCK_CHK_TYPE_QSPIN: return "qspinlock";
    case LOCK_CHK_TYPE_MUTEX: return "mutex";
    case LOCK_CHK_TYPE_MUTEX_SIMPLE: return "simple mutex";
    case LOCK_CHK_TYPE_RWLOCK: return "rwlock";
    }

    return "unknown lock";
}

void lock_debug_activate(void) {
    kassert(PERCPU_READY(lock_debug_cpu));
    atomic_store_explicit(&lock_debug_state, LOCK_CHK_ACTIVE,
                          memory_order_release);
}

void lock_debug_spin_classify(_Atomic uint8_t *usage,
                              enum lock_debug_irq_usage requested,
                              void *instance, enum lock_chk_type type,
                              const struct lock_chk_site *site) {
    if (atomic_load_explicit(&lock_debug_state, memory_order_acquire) !=
        LOCK_CHK_ACTIVE)
        return;

    uint8_t expected = LOCK_DEBUG_IRQ_NONE;
    if (atomic_compare_exchange_strong_explicit(usage, &expected, requested,
                                                memory_order_relaxed,
                                                memory_order_relaxed))
        return;

    if (expected != requested) {
        struct lock_chk_failure fail = {
            .kind = LOCK_CHK_FAIL_CONTEXT,
            .site = site,
            .instance = instance,
            .type = type,
        };
        lock_chk_fail(&fail, "%s %p changed IRQL usage",
                      lock_chk_type_name(type), instance);
    }
}

bool lock_debug_spin_push(void *instance, enum lock_chk_type type,
                          enum irql prev_irql,
                          const struct lock_chk_site *site) {
    if (atomic_load_explicit(&lock_debug_state, memory_order_acquire) !=
        LOCK_CHK_ACTIVE)
        return false;

    kassert(!are_interrupts_enabled());
    struct lock_debug_cpu *cpu = PERCPU_PTR(lock_debug_cpu);
    if (cpu->depth == LOCK_CHK_MAX_SPIN_DEPTH) {
        struct lock_chk_failure fail = {
            .kind = LOCK_CHK_FAIL_CAPACITY,
            .site = site,
            .instance = instance,
            .type = type,
            .capacity_pool = "shallow spin stack",
            .capacity_used = LOCK_CHK_MAX_SPIN_DEPTH,
            .capacity_limit = LOCK_CHK_MAX_SPIN_DEPTH,
        };
        lock_chk_fail(&fail, "Shallow spin stack capacity exhausted (%u/%u)",
                      LOCK_CHK_MAX_SPIN_DEPTH, LOCK_CHK_MAX_SPIN_DEPTH);
        atomic_store_explicit(&lock_debug_state, LOCK_CHK_DEGRADED,
                              memory_order_release);
        return false;
    }

    cpu->stack[cpu->depth++] = (struct lock_debug_spin_entry){
        .instance = instance,
        .acquire_site = site,
        .prev_irql = prev_irql,
        .type = type,
    };
    return true;
}

void lock_debug_spin_validate_top(void *instance, enum lock_chk_type type,
                                  enum irql prev_irql,
                                  const struct lock_chk_site *site) {
    if (atomic_load_explicit(&lock_debug_state, memory_order_acquire) !=
        LOCK_CHK_ACTIVE)
        return;

    kassert(!are_interrupts_enabled());
    struct lock_debug_cpu *cpu = PERCPU_PTR(lock_debug_cpu);
    if (cpu->depth == 0) {
        struct lock_chk_failure fail = {
            .kind = LOCK_CHK_FAIL_SPIN_ORDER,
            .site = site,
            .instance = instance,
            .type = type,
        };
        lock_chk_fail(&fail, "Releasing untracked %s %p",
                      lock_chk_type_name(type), instance);
        return;
    }

    struct lock_debug_spin_entry *top = &cpu->stack[cpu->depth - 1];
    if (top->instance != instance || top->type != type ||
        top->prev_irql != prev_irql) {
        struct lock_chk_failure fail = {
            .kind = LOCK_CHK_FAIL_SPIN_ORDER,
            .site = site,
            .instance = instance,
            .type = type,
        };
        lock_chk_fail(&fail, "Non-LIFO %s release %p", lock_chk_type_name(type),
                      instance);
    }
}

void lock_debug_spin_pop(void *instance, enum lock_chk_type type) {
    if (atomic_load_explicit(&lock_debug_state, memory_order_acquire) !=
        LOCK_CHK_ACTIVE)
        return;

    kassert(!are_interrupts_enabled());
    struct lock_debug_cpu *cpu = PERCPU_PTR(lock_debug_cpu);
    kassert(cpu->depth != 0);
    struct lock_debug_spin_entry *top = &cpu->stack[cpu->depth - 1];
    kassert(top->instance == instance);
    kassert(top->type == type);
    cpu->depth--;
}

#endif /* DEBUG_LOCK_CHK */
