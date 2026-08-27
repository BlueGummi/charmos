#include <compiler.h>
#include <kassert.h>
#include <smp/core.h>
#include <smp/percpu.h>
#include <sync/qspinlock.h>

struct qnode {
    _Atomic(struct qnode *) next;
    _Atomic uint8_t locked;
} __cache_aligned;

PERCPU_DECLARE(qnodes, struct qnode[QSPINLOCK_LEVEL_MAX], NULL);

/* The idea here: if we are running at DISPATCH, this lock
 * is also a DISPATCH lock, otherwise, this is a HIGH lock
 *
 * If we acquire a HIGH lock, we can also have a DISPATCH
 * lock sitting on a queue, so we use the separate qspinlock_level
 * so we don't reuse the qnode */
static enum qspinlock_level qspinlock_get_level() {
    if (irql_get() == IRQL_DISPATCH_LEVEL)
        return QSPINLOCK_LEVEL_NORMAL;

    /* QSPINLOCK_LEVEL_NMI is unsupported, we don't yet support
     * spinlocks in NMIs, and it's not planned */
    return QSPINLOCK_LEVEL_IRQ;
}

static uint32_t qspinlock_exchange_tail(struct qspinlock *lock, uint32_t tail,
                                        uint32_t val) {
    uint32_t next;

    do {
        next = (val & ~Q_SPIN_TAIL_MASK) | tail;
    } while (!atomic_compare_exchange_weak_explicit(
        &lock->val, &val, next, memory_order_acq_rel, memory_order_relaxed));

    return val;
}

void qspin_lock_slowpath(struct qspinlock *lock, uint32_t val) {

    /* No tail? Check the pending bit */
    if (!(val & Q_SPIN_TAIL_MASK)) {
        while (!(val & Q_SPIN_PENDING_MASK)) {
            uint32_t old = val;
            if (atomic_compare_exchange_weak_explicit(
                    &lock->val, &old, val | Q_SPIN_PENDING_VAL,
                    memory_order_acquire, memory_order_relaxed)) {

                /* We are the pender, now we wait for LOCK to clear */
                while ((val = atomic_load_explicit(&lock->val,
                                                   memory_order_relaxed)) &
                       Q_SPIN_LOCKED_MASK)
                    cpu_relax();

                /* pending -> locked: -0x100 + 1 */
                atomic_fetch_add_explicit(
                    &lock->val, Q_SPIN_LOCKED_VAL - Q_SPIN_PENDING_VAL,
                    memory_order_acquire);
                return;
            }
            val = old;
        }
    }

    cpu_id_t cpu = smp_core_id();

    /* Fallback if not ready */
    if (unlikely(!PERCPU_READY(qnodes))) {
        while (!qspin_trylock_physical(lock))
            cpu_relax();
        return;
    }

    enum qspinlock_level lvl = qspinlock_get_level();
    struct qnode *nodes = PERCPU_READ_FOR_CPU(qnodes, cpu);

    struct qnode *node = &nodes[lvl];
    atomic_store_explicit(&node->locked, 0, memory_order_relaxed);
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);

    /* Build the tail: We encode the level and the CPU */
    uint32_t tail =
        ((cpu + 1) << Q_SPIN_TAIL_CPU_OFFSET) | (lvl << Q_SPIN_TAIL_LVL_OFFSET);

    /* Publish the tail without overwriting a concurrent locked/pending update.
     */
    uint32_t old_val = qspinlock_exchange_tail(lock, tail, val);

    uint32_t old_tail = old_val & Q_SPIN_TAIL_MASK;
    if (old_tail) {
        cpu_id_t prev_cpu =
            ((old_tail & Q_SPIN_TAIL_CPU_MASK) >> Q_SPIN_TAIL_CPU_OFFSET) - 1;
        uint32_t prev_idx =
            (old_tail & Q_SPIN_TAIL_LVL_MASK) >> Q_SPIN_TAIL_LVL_OFFSET;

        struct qnode *prev_nodes = PERCPU_READ_FOR_CPU(qnodes, prev_cpu);
        struct qnode *prev_node = &prev_nodes[prev_idx];

        /* Chain us up */
        atomic_store_explicit(&prev_node->next, node, memory_order_release);

        /* The signal will propagate to us */
        while (!atomic_load_explicit(&node->locked, memory_order_acquire))
            cpu_relax();
    }

    /* We're at the head now, wait for pending, at this point no new CPU
     * will be able to set PENDING as they're failing on Q_SPIN_TAIL_MASK */
    while ((val = atomic_load_explicit(&lock->val, memory_order_relaxed)) &
           Q_SPIN_LOCKED_PENDING_MASK)
        cpu_relax();

    /* If no one new joined, clear the tail and claim the lock, or claim
     * lock + notify the successor to us */
    while (true) {
        if ((val & Q_SPIN_TAIL_MASK) == tail) {
            if (atomic_compare_exchange_weak_explicit(
                    &lock->val, &val, Q_SPIN_LOCKED_VAL, memory_order_acquire,
                    memory_order_relaxed))
                return; /* Got it */
        } else {
            atomic_fetch_or_explicit(&lock->val, Q_SPIN_LOCKED_VAL,
                                     memory_order_acquire);
            break;
        }
        cpu_relax();
    }

    /* successor links */
    while (!atomic_load_explicit(&node->next, memory_order_acquire))
        cpu_relax();

    struct qnode *next_node =
        atomic_load_explicit(&node->next, memory_order_relaxed);

    /* "Level-triggered" "notification" */
    atomic_store_explicit(&next_node->locked, 1, memory_order_release);
}
