#include <compiler/core.h>
#include <kassert.h>
#include <smp/core.h>
#include <smp/percpu.h>
#include <sync/qspinlock.h>

struct qnode {
    atomic(struct qnode *) next;
    atomic_uint8_t locked;
} cc_cache_aligned;

PERCPU_DEFINE(struct qnode[QSPINLOCK_LEVEL_MAX], qnodes);

/* The idea here: if we are running at DISPATCH, this lock
 * is also a DISPATCH lock, otherwise, this is a HIGH lock
 *
 * If we acquire a HIGH lock, we can also have a DISPATCH
 * lock sitting on a queue, so we use the separate qspinlock_level
 * so we don't reuse the qnode */
static enum qspinlock_level qspinlock_get_level(void) {
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
    } while (!atomic_cas_weak(&lock->val, &val, next, mo_acq_rel, mo_rlx));

    return val;
}

void qspin_lock_slowpath(struct qspinlock *lock, uint32_t val) {

    /* No tail? Check the pending bit */
    if (!(val & Q_SPIN_TAIL_MASK)) {
        while (!(val & Q_SPIN_PENDING_MASK)) {
            uint32_t old = val;
            if (atomic_cas_weak(&lock->val, &old, val | Q_SPIN_PENDING_VAL,
                                mo_acquire, mo_rlx)) {

                /* We are the pender, now we wait for LOCK to clear */
                while ((val = atomic_load_rlx(&lock->val)) & Q_SPIN_LOCKED_MASK)
                    cpu_pause();

                /* pending -> locked: -0x100 + 1 */
                atomic_fetch_add_acq(&lock->val,
                                     Q_SPIN_LOCKED_VAL - Q_SPIN_PENDING_VAL);
                return;
            }
            val = old;
        }
    }

    cpu_id_t cpu = smp_id(TOPC_IRQL);

    /* Fallback if not ready */
    if (cc_unlikely(!PERCPU_READY(qnodes))) {
        while (!qspin_trylock_physical(lock))
            cpu_pause();
        return;
    }

    enum qspinlock_level lvl = qspinlock_get_level();
    struct qnode *nodes = PERCPU_READ_FOR(qnodes, cpu);

    struct qnode *node = &nodes[lvl];
    atomic_store_rlx(&node->locked, 0);
    atomic_store_rlx(&node->next, NULL);

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

        struct qnode *prev_nodes = PERCPU_READ_FOR(qnodes, prev_cpu);
        struct qnode *prev_node = &prev_nodes[prev_idx];

        /* Chain us up */
        atomic_store_release(&prev_node->next, node);

        /* The signal will propagate to us */
        while (!atomic_load_acq(&node->locked))
            cpu_pause();
    }

    /* We're at the head now, wait for pending, at this point no new CPU
     * will be able to set PENDING as they're failing on Q_SPIN_TAIL_MASK */
    while ((val = atomic_load_rlx(&lock->val)) & Q_SPIN_LOCKED_PENDING_MASK)
        cpu_pause();

    /* If no one new joined, clear the tail and claim the lock, or claim
     * lock + notify the successor to us */
    while (true) {
        if ((val & Q_SPIN_TAIL_MASK) == tail) {
            if (atomic_cas_weak(&lock->val, &val, Q_SPIN_LOCKED_VAL, mo_acquire,
                                mo_rlx))
                return; /* Got it */
        } else {
            atomic_fetch_or_acq(&lock->val, Q_SPIN_LOCKED_VAL);
            break;
        }
        cpu_pause();
    }

    /* successor links */
    while (!atomic_load_acq(&node->next))
        cpu_pause();

    struct qnode *next_node = atomic_load_rlx(&node->next);

    /* "Level-triggered" "notification" */
    atomic_store_release(&next_node->locked, 1);
}
