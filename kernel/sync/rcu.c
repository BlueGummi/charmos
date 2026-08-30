/*
 * Preemptible tree RCU is what we do here
 *
 * The big invariant here: Grace periods have to outlive
 * all read sections that began *before* it started
 *
 * How we do that:
 *
 * 1.  CPUs report quiescent states for grace period N only after it's
 *     observed gp_seq == N. Read sections that begin on that CPU
 *     after the fact thus can't see pointers that the batch will free
 *
 * 2.  CPUs never report while unregistered readers are still active,
 *     but once a reader switches out, it is registered on the leaf
 *
 * Quiescent states combine up a tree, allowing us to complete grace periods
 * in O(CPUs / fanout + preempted readers)
 *
 * TODO: CPU hotplug, expedited grace periods, priority boosting blocked
 * readers, and maybe a better watchdog integration under a debug flag
 * to get more information about what sleeping readers are doing
 */

#include <acpi/lapic.h>
#include <console/printf.h>
#include <global.h>
#include <irq/irq.h>
#include <kassert.h>
#include <log.h>
#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <structures/list.h>
#include <sync/rcu.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>
#include <thread/thread.h>
#include <time/spin_sleep.h>
#include <time/time.h>

#include "rcu_internal.h"

LOG_SITE_DECLARE_PRINT(rcu);
LOG_HANDLE_DECLARE_PRINT(rcu);
static struct rcu_state rcu;

static inline struct rcu_node *rcu_leaf_for_cpu(cpu_id_t cpu) {
    return &rcu.leaves[cpu / RCU_FANOUT];
}

static inline bool rcu_node_pending(const struct rcu_node *node) {
    if (node->is_leaf)
        return !cpu_mask_empty(&node->qs_cpus) || node->blocked_count != 0;

    return node->qs_children != 0;
}

/* Send completed node up
 *
 * A node completes when it's empty, with no child mask bits,
 * no readers for this GP, and completed_seq keeps this idempotent
 */
static void rcu_propagate_done(struct rcu_node *node, uint64_t seq,
                               enum irql irql) {
    SPINLOCK_ASSERT_HELD(&node->lock);
    while (true) {
        if (node->gp_seq != seq || node->completed_seq == seq ||
            rcu_node_pending(node)) {
            spin_unlock(&node->lock, irql);
            return;
        }

        kassert(seq > node->completed_seq);
        node->completed_seq = seq;

        struct rcu_node *parent = node->parent;
        size_t child_index = node->child_index;

        spin_unlock(&node->lock, irql);

        if (!parent) {
            /* Root is done, GP over */
            atomic_store_explicit(&rcu.gp_completed, seq, memory_order_release);
            return;
        }

        irql = spin_lock_irq_disable(&parent->lock);
        if (parent->gp_seq != seq) {
            spin_unlock(&parent->lock, irql);
            return;
        }

        bitmap_clear(&parent->qs_children, child_index);
        node = parent;
    }
}

/* cpu has passed through a quiescent state */
static void rcu_report_cpu_locked(struct rcu_node *leaf, cpu_id_t cpu,
                                  uint64_t gp_seq_seen, enum irql irql) {
    SPINLOCK_ASSERT_HELD(&leaf->lock);
    if (leaf->gp_seq != gp_seq_seen || leaf->completed_seq == gp_seq_seen) {
        spin_unlock(&leaf->lock, irql);
        return;
    }

    atomic_store_explicit(&rcu.cpus[cpu].reported_seq, gp_seq_seen,
                          memory_order_relaxed);

    cpu_mask_clear(&leaf->qs_cpus, cpu);
    rcu_propagate_done(leaf, gp_seq_seen, irql);
}

void rcu_read_lock(void) {
    struct thread *t = thread_get_current();
    if (unlikely(!t))
        return;

    kassert_debug(!irq_in_nmi(), "RCU read section in NMI context");

    if (atomic_load_explicit(&t->rcu_nesting, memory_order_relaxed) == 0) {
        uint64_t seq = atomic_load_explicit(&rcu.gp_seq, memory_order_acquire);
        atomic_store_explicit(&t->rcu_read_seq, seq, memory_order_relaxed);
    }

    /* TODO: There should be a smart way that avoids seq_cst ordering...
     *
     * But we do this so that this can't come after the first
     * rcu_dereference(), and because I will optimize later */
    uint32_t old =
        atomic_fetch_add_explicit(&t->rcu_nesting, 1, memory_order_seq_cst);

    kassert(old != UINT32_MAX, "RCU nesting overflow");
}

/* Remove a reader off the leaf, and this can run on whatever CPU
 * the reader happens to wake on */
static void rcu_unregister_reader(struct thread *t) {
    enum irql outer = irql_raise(IRQL_HIGH_LEVEL);

    struct rcu_node *leaf = t->rcu_leaf;
    if (leaf) {
        enum irql irql = spin_lock_irq_disable(&leaf->lock);

        list_del_init(&t->rcu_list_node);
        t->rcu_leaf = NULL;

        uint64_t bseq = t->rcu_blocked_seq;
        t->rcu_blocked_seq = 0;

        if (bseq != 0 && bseq == leaf->gp_seq) {
            kassert(leaf->blocked_count != 0, "RCU blocked reader underflow");
            leaf->blocked_count--;
            rcu_propagate_done(leaf, bseq, irql);
        } else {
            spin_unlock(&leaf->lock, irql);
        }
    }

    irql_lower(outer);
}

void rcu_read_unlock(void) {
    struct thread *t = thread_get_current();
    if (unlikely(!t))
        return;

    uint32_t old =
        atomic_fetch_sub_explicit(&t->rcu_nesting, 1, memory_order_seq_cst);
    kassert(old != 0, "RCU nesting underflow");

    /* Nesting is zero, we're registered, unregister
     *
     * We do this AFTER decrement so if we get preempted in this window,
     * the scheduler just sees an idle reader and moves on */
    if (unlikely(old == 1 && t->rcu_leaf))
        rcu_unregister_reader(t); /* NOTE: This can potentially result in cross
                                   * node traffic and cache unhappiness, but
                                   * this is also the "very slow path"....
                                   *
                                   * Perhaps something can be done (?)
                                   */
}

/* We might want to change next_is_idle to a thread pointer...
 *
 * Lock ordering here is scheduler -> leaf -> parents */
void rcu_note_context_switch(struct thread *outgoing, bool next_is_idle) {
    if (!unlikely(rcu.ready))
        return;

    enum irql outer = irql_raise(IRQL_HIGH_LEVEL);

    cpu_id_t cpu = smp_core_id();
    struct rcu_node *leaf = rcu_leaf_for_cpu(cpu);
    uint64_t gp_seq_seen =
        atomic_load_explicit(&rcu.gp_seq, memory_order_acquire);

    enum irql irql = spin_lock_irq_disable(&leaf->lock);

    if (next_is_idle)
        cpu_mask_set(&leaf->idle_cpus, cpu);
    else
        cpu_mask_clear(&leaf->idle_cpus, cpu);

    /* Registration uses the leaf's tracked seq, NOT gp_seq_seen
     *
     * Grace period starts initialize leaves before publishing the
     * sequence, and readers that get switched out in the window
     * come before the new GP, although this CPU hasn't seen the publish */
    uint64_t lseq = leaf->gp_seq;
    bool leaf_active = leaf->completed_seq != lseq;

    if (outgoing &&
        atomic_load_explicit(&outgoing->rcu_nesting, memory_order_relaxed) !=
            0 &&
        !outgoing->rcu_leaf) {
        outgoing->rcu_leaf = leaf;
        outgoing->rcu_blocked_seq = 0;
        list_add_tail(&outgoing->rcu_list_node, &leaf->blocked);

        uint64_t rseq =
            atomic_load_explicit(&outgoing->rcu_read_seq, memory_order_relaxed);
        if (leaf_active && rseq < lseq) {
            outgoing->rcu_blocked_seq = lseq;
            leaf->blocked_count++;
        }
    }

    rcu_report_cpu_locked(leaf, cpu, gp_seq_seen, irql);

    irql_lower(outer);
}

/* This just lets the CPU answer IRQ_NOP */
void rcu_note_irq_exit(void) {
    if (!rcu.ready)
        return;

    kassert(irq_in_interrupt());
    struct thread *t = thread_get_current();

    /* Just report when the interrupted context isn't holding an unregistered
     * read side critical section, as readers that are accounted in a leaf
     * don't need to be handled over here */
    if (t && atomic_load_explicit(&t->rcu_nesting, memory_order_relaxed) != 0 &&
        !t->rcu_leaf)
        return;

    uint64_t gp_seq_seen =
        atomic_load_explicit(&rcu.gp_seq, memory_order_acquire);
    if (gp_seq_seen ==
        atomic_load_explicit(&rcu.gp_completed, memory_order_relaxed))
        return;

    cpu_id_t cpu = smp_core_id();
    if (atomic_load_explicit(&rcu.cpus[cpu].reported_seq,
                             memory_order_relaxed) == gp_seq_seen)
        return;

    /* Technically the IRQL stuff here is a no-op, but we'll
     * preserve it in case we move the irq.c callsite +
     * gives us a bit of semantic/invariant clarity */
    enum irql outer = irql_raise(IRQL_HIGH_LEVEL);

    struct rcu_node *leaf = rcu_leaf_for_cpu(cpu);

    enum irql irql = spin_lock_irq_disable(&leaf->lock);
    rcu_report_cpu_locked(leaf, cpu, gp_seq_seen, irql);

    irql_lower(outer);
}

void rcu_defer(struct rcu_cb *cb, rcu_fn func, void *arg) {
    kassert(cb && func);

    cb->fn = func;
    cb->arg = arg;
    cb->target_gen = 0;
    cb->gen_when_called = 0;
    INIT_LIST_HEAD(&cb->list);

    /* Pin + disable interrupts for queue selection */
    enum irql outer = irql_raise(IRQL_HIGH_LEVEL);

    cb->enqueued_waiting_on_gen =
        (size_t) atomic_load_explicit(&rcu.gp_seq, memory_order_relaxed);

    if (unlikely(!rcu.ready)) {
        /* Before rcu_init(), we have no tree, no worker, and the boot CPU
         * is the only thing running RCU operations, so we can just do this */
        irql_lower(outer);
        cb->fn(cb, cb->arg);
        return;
    }

    struct rcu_cpu *q = &rcu.cpus[smp_core_id()];

    enum irql irql = spin_lock_irq_disable(&q->lock);
    list_add_tail(&cb->list, &q->list);
    spin_unlock(&q->lock, irql);

    irql_lower(outer);

    semaphore_post(&rcu.sem);
}

static void rcu_detach_callbacks(struct list_head *batch) {
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++) {
        struct rcu_cpu *q = &rcu.cpus[cpu];

        enum irql irql = spin_lock_irq_disable(&q->lock);
        list_splice_tail_init(&q->list, batch);
        spin_unlock(&q->lock, irql);
    }
}

static bool rcu_callbacks_pending(void) {
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++) {
        struct rcu_cpu *q = &rcu.cpus[cpu];

        enum irql irql = spin_lock_irq_disable(&q->lock);
        bool empty = list_empty(&q->list);
        spin_unlock(&q->lock, irql);

        if (!empty)
            return true;
    }
    return false;
}

static bool rcu_work_pending(void) {
    if (atomic_load_explicit(&rcu.gp_requests, memory_order_acquire) != 0)
        return true;
    return rcu_callbacks_pending();
}

/* This runs at PASSIVE so callbacks can do whatever */
static void rcu_run_batch(struct list_head *batch, uint64_t seq) {
    struct rcu_cb *cb, *tmp;
    list_for_each_entry_safe(cb, tmp, batch, list) {
        list_del_init(&cb->list);
        cb->gen_when_called = (size_t) seq;
        cb->fn(cb, cb->arg);
    }
}

/* Bother everyone who's not quiesced, skip idlers */
static void rcu_kick_pending(uint64_t seq) {
    cpu_id_t self = smp_core_id();

    for (size_t l = 0; l < rcu.leaf_count; l++) {
        struct rcu_node *leaf = &rcu.leaves[l];

        struct cpu_mask pending = CPU_MASK_INIT;

        enum irql irql = spin_lock_irq_disable(&leaf->lock);
        if (leaf->gp_seq == seq)
            cpu_mask_copy(&pending, &leaf->qs_cpus);
        spin_unlock(&leaf->lock, irql);

        cpu_id_t cpu;
        for_each_cpu(cpu, &pending) {
            if (cpu != self)
                ipi_send((uint32_t) cpu, IRQ_NOP);
        }
    }
}

/* TODO: nonfatal, we'll need to do something a bit smarter here with logging/
 * errors for parseability and whatnot */
static void rcu_report_stall(uint64_t seq, time_ms_t elapsed) {
    struct rcu_stall_blocker blockers[RCU_STALL_MAX_REPORTED];
    size_t nblockers = 0;

    rcu_warn("grace period %llu stalled for %llu ms\n",
             (unsigned long long) seq, (unsigned long long) elapsed);

    for (size_t l = 0; l < rcu.leaf_count; l++) {
        struct rcu_node *leaf = &rcu.leaves[l];

        struct cpu_mask pending = CPU_MASK_INIT;

        enum irql irql = spin_lock_irq_disable(&leaf->lock);

        if (leaf->gp_seq == seq)
            cpu_mask_copy(&pending, &leaf->qs_cpus);
        uint32_t blocked = leaf->blocked_count;

        struct thread *t;
        list_for_each_entry(t, &leaf->blocked, rcu_list_node) {
            if (t->rcu_blocked_seq != seq)
                continue;
            if (nblockers >= RCU_STALL_MAX_REPORTED)
                break;

            blockers[nblockers++] = (struct rcu_stall_blocker){
                .id = t->id,
                .name = t->name,
                .state = (int) thread_get_state(t),
                .read_seq = atomic_load_explicit(&t->rcu_read_seq,
                                                 memory_order_relaxed),
            };
        }

        spin_unlock(&leaf->lock, irql);

        if (!cpu_mask_empty(&pending) || blocked)
            rcu_warn("leaf %zu: cpus %#llx pending, %u blocking reader(s)\n", l,
                     (unsigned long long)
                         pending.bits[leaf->cpu_base / BITMAP_BITS_PER_WORD],
                     blocked);
    }

    for (size_t i = 0; i < nblockers; i++)
        rcu_warn("tid %llu \"%s\" state %d read_seq %llu\n",
                 (unsigned long long) blockers[i].id,
                 blockers[i].name ? blockers[i].name : "?", blockers[i].state,
                 (unsigned long long) blockers[i].read_seq);
}

static uint64_t rcu_gp_start(struct list_head *batch) {
    rcu_detach_callbacks(batch);
    atomic_store_explicit(&rcu.gp_requests, 0, memory_order_relaxed);

    uint64_t prev = atomic_load_explicit(&rcu.gp_seq, memory_order_relaxed);
    kassert(prev != UINT64_MAX, "RCU GP seq wrap");
    uint64_t seq = prev + 1;

    struct rcu_cb *cb;
    list_for_each_entry(cb, batch, list) cb->target_gen = (size_t) seq;

    /* Root down, rcu.nodes is root then leaves */
    for (size_t i = 0; i < rcu.node_count; i++) {
        struct rcu_node *node = &rcu.nodes[i];

        enum irql irql = spin_lock_irq_disable(&node->lock);
        node->gp_seq = seq;
        node->qs_children = node->full_children;

        if (node->is_leaf) {
            cpu_mask_copy(&node->qs_cpus, &node->full_cpus);
            node->blocked_count = 0;

            /* Everyone here began before the GP */
            struct thread *t;
            list_for_each_entry(t, &node->blocked, rcu_list_node) {
                uint64_t rseq = atomic_load_explicit(&t->rcu_read_seq,
                                                     memory_order_relaxed);
                if (rseq < seq) {
                    t->rcu_blocked_seq = seq;
                    node->blocked_count++;
                } else {
                    t->rcu_blocked_seq = 0;
                }
            }
        }
        spin_unlock(&node->lock, irql);
    }

    atomic_store_explicit(&rcu.gp_seq, seq, memory_order_release);

    /* Retire quiescent and poke everyone else */
    cpu_id_t self = smp_core_id();

    for (size_t l = 0; l < rcu.leaf_count; l++) {
        struct rcu_node *leaf = &rcu.leaves[l];

        enum irql irql = spin_lock_irq_disable(&leaf->lock);
        if (leaf->gp_seq != seq) {
            spin_unlock(&leaf->lock, irql);
            continue;
        }

        cpu_mask_andnot(&leaf->qs_cpus, &leaf->qs_cpus, &leaf->idle_cpus);
        if (leaf == rcu_leaf_for_cpu(self))
            cpu_mask_clear(&leaf->qs_cpus, self);

        struct cpu_mask pending;
        cpu_mask_copy(&pending, &leaf->qs_cpus);
        rcu_propagate_done(leaf, seq, irql);

        cpu_id_t cpu;
        for_each_cpu(cpu, &pending) ipi_send((uint32_t) cpu, IRQ_NOP);
    }

    return seq;
}

static void rcu_gp_wait(uint64_t seq) {
    time_ms_t started = time_get_ms();
    time_ms_t last_kick = started;
    time_ms_t last_stall = started;

    while (atomic_load_explicit(&rcu.gp_completed, memory_order_acquire) <
           seq) {
        scheduler_yield();

        time_ms_t now = time_get_ms();

        if (now - last_kick >= RCU_KICK_INTERVAL_MS) {
            last_kick = now;
            rcu_kick_pending(seq);
        }

        if (now - last_stall >= RCU_STALL_MS) {
            last_stall = now;
            rcu_report_stall(seq, now - started);
        }
    }
}

static void rcu_gp_worker(void *unused_arg) {
    unused(unused_arg);

    while (true) {
        semaphore_wait(&rcu.sem);

        if (!rcu_work_pending())
            continue;

        struct list_head batch;
        INIT_LIST_HEAD(&batch);

        uint64_t seq = rcu_gp_start(&batch);
        rcu_gp_wait(seq);
        rcu_run_batch(&batch, seq);
    }
}

void rcu_synchronize(void) {
    struct thread *t = thread_get_current();

    kassert(!irq_in_interrupt() && !irq_in_nmi(),
            "rcu_synchronize() from interrupt context");
    kassert(irql_get() <= IRQL_APC_LEVEL, "rcu_synchronize() above APC level");
    kassert(!t || atomic_load_explicit(&t->rcu_nesting, memory_order_relaxed) ==
                      0,
            "rcu_synchronize() inside an RCU read section");
    kassert(!rcu.ready || t != rcu.worker,
            "rcu_synchronize() from the RCU worker deadlocks");

    if (!rcu.ready)
        return;

    /* Caller's unpublish happened before this load, meaning
     * grace periods that published a sequence greater than what's
     * here started after the unpublish */
    uint64_t target =
        atomic_load_explicit(&rcu.gp_seq, memory_order_acquire) + 1;

    atomic_fetch_add_explicit(&rcu.gp_requests, 1, memory_order_release);
    semaphore_post(&rcu.sem);

    while (atomic_load_explicit(&rcu.gp_completed, memory_order_acquire) <
           target)
        scheduler_yield();
}

static void rcu_build_tree(void) {
    size_t counts[RCU_MAX_LEVELS];
    size_t levels = 0;

    size_t n = global.core_count;
    kassert(n > 0);

    do {
        n = (n + RCU_FANOUT - 1) / RCU_FANOUT;
        kassert(levels < RCU_MAX_LEVELS, "RCU tree deeper than RCU_MAX_LEVELS");
        counts[levels++] = n;
    } while (n > 1);

    /* counts[0] is the leaf, counts[levels - 1] is the root */
    size_t total = 0;
    for (size_t i = 0; i < levels; i++)
        total += counts[i];

    rcu.nodes =
        kmalloc_or_die(total * sizeof(struct rcu_node), ALLOC_FLAGS_ZERO);
    rcu.node_count = total;

    /* Root first so forward pass initializes parents before children */
    size_t offsets[RCU_MAX_LEVELS];
    offsets[levels - 1] = 0;
    for (size_t i = levels - 1; i > 0; i--)
        offsets[i - 1] = offsets[i] + counts[i];

    for (size_t level = 0; level < levels; level++) {
        for (size_t j = 0; j < counts[level]; j++) {
            struct rcu_node *node = &rcu.nodes[offsets[level] + j];

            spinlock_init(&node->lock);
            node->gp_seq = 0;
            node->completed_seq = 0;
            node->qs_children = 0;

            if (level == levels - 1) {
                node->parent = NULL;
                node->child_index = 0;
            } else {
                node->parent =
                    &rcu.nodes[offsets[level + 1] + (j / RCU_FANOUT)];
                node->child_index = j % RCU_FANOUT;
            }

            size_t owned;
            if (level == 0) {
                node->is_leaf = true;
                node->cpu_base = j * RCU_FANOUT;
                INIT_LIST_HEAD(&node->blocked);
                owned = global.core_count - node->cpu_base;
            } else {
                node->is_leaf = false;
                owned = counts[level - 1] - (j * RCU_FANOUT);
            }

            if (owned > RCU_FANOUT)
                owned = RCU_FANOUT;

            if (node->is_leaf)
                cpu_mask_set_range(&node->full_cpus, node->cpu_base, owned);
            else
                bitmap_fill(&node->full_children, owned);
        }
    }

    rcu.root = &rcu.nodes[0];
    rcu.leaves = &rcu.nodes[offsets[0]];
    rcu.leaf_count = counts[0];
}

void rcu_init(void) {
    atomic_store(&rcu.gp_seq, 1);
    atomic_store(&rcu.gp_completed, 1);
    atomic_store(&rcu.gp_requests, 0);

    semaphore_init(&rcu.sem, 0, SEMAPHORE_INIT_NORMAL);

    rcu_build_tree();

    rcu.cpus = kmalloc_or_die(global.core_count * sizeof(struct rcu_cpu),
                              ALLOC_FLAGS_ZERO);
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++) {
        spinlock_init(&rcu.cpus[cpu].lock);
        INIT_LIST_HEAD(&rcu.cpus[cpu].list);
        atomic_store(&rcu.cpus[cpu].reported_seq, 0);
    }

    rcu.worker = thread_spawn("rcu_gp_worker", rcu_gp_worker, NULL);
    rcu.ready = true;
}
