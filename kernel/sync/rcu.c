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

#include <atomic.h>
#include <compiler/atomic.h>
#include <console/printf.h>
#include <enforce.h>
#include <global.h>
#include <irq/ipi.h>
#include <irq/irq.h>
#include <log.h>
#include <math/align.h>
#include <math/min_max.h>
#include <mem/alloc.h>
#include <mem/nofail.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <structures/list.h>
#include <sync/rcu.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>
#include <thread/thread.h>
#include <time/spin_sleep.h>
#include <time/time.h>

#include "rcu_internal.h"

LOG_SITE_DEFINE_PRINT(rcu);
LOG_HANDLE_DEFINE_PRINT(rcu);
static struct rcu_globals rcu_global;

static inline struct rcu_node *rcu_leaf_for_cpu(cpu_id_t cpu) {
    return &rcu_global.leaves[cpu / RCU_FANOUT];
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
                               enum irql irql) TSA_NO_ANALYSIS {
    SPINLOCK_ASSERT_HELD(&node->lock);
    while (true) {
        if (node->gp_seq != seq || node->completed_seq == seq ||
            rcu_node_pending(node)) {
            spin_unlock(&node->lock, irql);
            return;
        }

        enforce(seq > node->completed_seq);
        node->completed_seq = seq;

        struct rcu_node *parent = node->parent;
        size_t child_index = node->child_index;

        spin_unlock(&node->lock, irql);

        if (!parent) {
            /* Root is done, GP over */
            atomic_store_release(&rcu_global.gp_completed, seq);
            return;
        }

        irql = spin_lock_high(&parent->lock);
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
                                  uint64_t gp_seq_seen,
                                  enum irql irql) TSA_NO_ANALYSIS {
    SPINLOCK_ASSERT_HELD(&leaf->lock);
    if (leaf->gp_seq != gp_seq_seen || leaf->completed_seq == gp_seq_seen) {
        spin_unlock(&leaf->lock, irql);
        return;
    }

    atomic_store_rlx(&rcu_global.cpus[cpu].reported_seq, gp_seq_seen);

    cpu_mask_clear(&leaf->qs_cpus, cpu);
    rcu_propagate_done(leaf, gp_seq_seen, irql);
}

void rcu_read_lock(void) TSA_NO_ANALYSIS {
    struct thread *t = thread_get_current();
    if (cc_unlikely(!t))
        return;

    debug_enforce(!irq_in_nmi(), "RCU read section in NMI context");

    uint32_t nesting = ca_read_once(t->rcu_nesting);
    enforce(nesting != UINT32_MAX, "RCU nesting overflow");

    if (cc_likely(nesting != 0)) {
        ca_write_once(t->rcu_nesting, nesting + 1);
    } else {
        uint64_t seq = atomic_load_acq(&rcu_global.gp_seq);
        t->rcu_read_seq = seq;
        ca_write_once(t->rcu_nesting, 1);
    }

    ca_barrier();
    crash_unwind_enter_rcu();
}

/* Remove a reader off the leaf, and this can run on whatever CPU
 * the reader happens to wake on */
static void rcu_unregister_reader(struct thread *t) TSA_NO_ANALYSIS {
    enum irql outer = irql_raise(IRQL_HIGH_LEVEL);

    struct rcu_node *leaf = t->rcu_leaf;
    if (leaf) {
        enum irql irql = spin_lock_high(&leaf->lock);

        list_node_delete_init(&t->rcu_list_node);
        t->rcu_leaf = NULL;

        uint64_t bseq = t->rcu_blocked_seq;
        t->rcu_blocked_seq = 0;

        if (bseq != 0 && bseq == leaf->gp_seq) {
            enforce(leaf->blocked_count != 0, "RCU blocked reader underflow");
            leaf->blocked_count--;
            rcu_propagate_done(leaf, bseq, irql);
        } else {
            spin_unlock(&leaf->lock, irql);
        }
    }

    irql_lower(outer);
}

void rcu_read_unlock(void) TSA_NO_ANALYSIS {
    struct thread *t = thread_get_current();
    if (cc_unlikely(!t))
        return;

    uint32_t nesting = ca_read_once(t->rcu_nesting);
    enforce(nesting != 0, "RCU nesting underflow");

    if (cc_likely(nesting > 1)) {
        ca_write_once(t->rcu_nesting, nesting - 1);
    } else {
        ca_barrier();
        ca_write_once(t->rcu_nesting, 0);
        ca_barrier();

        if (cc_unlikely(t->rcu_leaf))
            rcu_unregister_reader(t);
    }

    crash_unwind_exit_rcu();
}

static inline bool rcu_cpu_needs_qs(cpu_id_t cpu, uint64_t *seen) {
    uint64_t s = atomic_load_acq(&rcu_global.gp_seq);
    if (s == atomic_load_acq(&rcu_global.gp_completed))
        return false; /* no GP in flight */

    if (atomic_load_rlx(&rcu_global.cpus[cpu].reported_seq) == s)
        return false; /* already reported */

    *seen = s;
    return true;
}

void rcu_note_context_switch(struct thread *outgoing) TSA_NO_ANALYSIS {
    if (!cc_unlikely(rcu_global.ready))
        return;

    bool must_register =
        outgoing && ca_read_once(outgoing->rcu_nesting) && !outgoing->rcu_leaf;

    uint64_t seen;
    if (cc_likely(!must_register) &&
        !rcu_cpu_needs_qs(smp_id(TOPC_IRQL), &seen))
        return;

    enum irql outer = irql_raise(IRQL_HIGH_LEVEL);

    cpu_id_t cpu = smp_id(TOPC_IRQL);
    struct rcu_node *leaf = rcu_leaf_for_cpu(cpu);
    uint64_t gp_seq_seen = atomic_load_acq(&rcu_global.gp_seq);

    enum irql irql = spin_lock_high(&leaf->lock);

    uint64_t lseq = leaf->gp_seq;
    bool leaf_active = leaf->completed_seq != lseq;

    if (outgoing && ca_read_once(outgoing->rcu_nesting) &&
        !outgoing->rcu_leaf) {
        outgoing->rcu_leaf = leaf;
        outgoing->rcu_blocked_seq = 0;
        list_head_push_back(&leaf->blocked, &outgoing->rcu_list_node);

        uint64_t rseq = outgoing->rcu_read_seq;
        if (leaf_active && rseq < lseq) {
            outgoing->rcu_blocked_seq = lseq;
            leaf->blocked_count++;
        }
    }

    rcu_report_cpu_locked(leaf, cpu, gp_seq_seen, irql);

    irql_lower(outer);
}

/* This just lets the CPU answer IRQ_NOP */
void rcu_note_irq_exit(void) TSA_NO_ANALYSIS {
    if (!rcu_global.ready)
        return;

    enforce(irq_in_interrupt());
    struct thread *t = thread_get_current();

    /* Just report when the interrupted context isn't holding an unregistered
     * read side critical section, as readers that are accounted in a leaf
     * don't need to be handled over here */
    if (t && ca_read_once(t->rcu_nesting) && !t->rcu_leaf)
        return;

    uint64_t gp_seq_seen = atomic_load_acq(&rcu_global.gp_seq);
    if (gp_seq_seen == atomic_load_acq(&rcu_global.gp_completed))
        return;

    cpu_id_t cpu = smp_id(TOPC_IRQ);
    if (atomic_load_rlx(&rcu_global.cpus[cpu].reported_seq) == gp_seq_seen)
        return;

    /* Technically the IRQL stuff here is a no-op, but we'll
     * preserve it in case we move the irq.c callsite +
     * gives us a bit of semantic/invariant clarity */
    enum irql outer = irql_raise(IRQL_HIGH_LEVEL);

    struct rcu_node *leaf = rcu_leaf_for_cpu(cpu);

    enum irql irql = spin_lock_high(&leaf->lock);
    rcu_report_cpu_locked(leaf, cpu, gp_seq_seen, irql);

    irql_lower(outer);
}

void rcu_idle_enter(void) {
    if (!rcu_global.ready)
        return;

    enforce(!irqs_enabled());

    atomic_store_release(&rcu_global.cpus[smp_id(TOPC_IFLAG)].idle, true);
}

void rcu_idle_exit(void) {
    if (!rcu_global.ready)
        return;

    struct rcu_cpu *me = &rcu_global.cpus[smp_id(TOPC_IFLAG)];
    if (cc_likely(!atomic_load_rlx(&me->idle)))
        return;

    atomic_xchg_acq_rel(&me->idle, false);
}

void rcu_defer(struct rcu_cb *cb, rcu_fn func) {
    enforce(cb && func);

    cb->fn = func;

#ifdef TEST_ENABLED
    cb->target_gen = 0;
    cb->gen_when_called = 0;
#endif

    list_node_init(&cb->list);

    /* Pin + disable interrupts for queue selection */
    enum irql outer = irql_raise(IRQL_HIGH_LEVEL);

#ifdef TEST_ENABLED
    cb->enqueued_waiting_on_gen = atomic_load_rlx(&rcu_global.gp_seq);
#endif

    if (cc_unlikely(!rcu_global.ready)) {
        /* Before rcu_init(), we have nothing, and the boot CPU
         * is the only thing running RCU operations, so we can just do this */
        irql_lower(outer);
        cb->fn(cb);
        return;
    }

    struct rcu_cpu *q = &rcu_global.cpus[smp_id(TOPC_IRQL)];

    enum irql irql = spin_lock_high(&q->lock);
    list_head_push_back(&q->list, &cb->list);
    spin_unlock(&q->lock, irql);

    irql_lower(outer);

    semaphore_post(&rcu_global.sem);
}

static void rcu_detach_callbacks(struct list_head *batch) {
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++) {
        struct rcu_cpu *q = &rcu_global.cpus[cpu];

        enum irql irql = spin_lock_high(&q->lock);
        list_head_splice_back_init(batch, &q->list);
        spin_unlock(&q->lock, irql);
    }
}

static bool rcu_callbacks_pending(void) {
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++) {
        struct rcu_cpu *q = &rcu_global.cpus[cpu];

        enum irql irql = spin_lock_high(&q->lock);
        bool empty = list_head_empty(&q->list);
        spin_unlock(&q->lock, irql);

        if (!empty)
            return true;
    }
    return false;
}

static bool rcu_work_pending(void) {
    if (atomic_load_acq(&rcu_global.gp_requests) != 0)
        return true;
    return rcu_callbacks_pending();
}

/* This runs at PASSIVE so callbacks can do whatever */
static void rcu_run_batch(struct list_head *batch, uint64_t seq) {
    struct rcu_cb *cb, *tmp;
    list_for_each_entry_safe(cb, tmp, batch, list) {
        list_node_delete_init(&cb->list);

#ifdef TEST_ENABLED
        cb->gen_when_called = (size_t) seq;
#endif

        cb->fn(cb);
    }
}

/* Bother everyone who's not quiesced
 *
 * CPUs that went to hlt after rcu_gp_start sampled them still need
 * a QS. Instead of IPI'ing just for them to say they're idle,
 * we check the percpu flag */
static void rcu_kick_pending(uint64_t seq) TSA_NO_ANALYSIS {
    /* Pairs with xchg in rcu_idle_exit */
    cpu_memory_barrier();

    for (size_t l = 0; l < rcu_global.leaf_count; l++) {
        struct rcu_node *leaf = &rcu_global.leaves[l];

        enum irql irql = spin_lock_high(&leaf->lock);

        if (leaf->gp_seq != seq) {
            spin_unlock(&leaf->lock, irql);
            continue;
        }

        struct cpu_mask owing;
        cpu_mask_copy(&owing, &leaf->qs_cpus);

        cpu_id_t cpu;
        for_each_cpu(cpu, &owing) {
            if (!atomic_load_acq(&rcu_global.cpus[cpu].idle))
                continue;

            cpu_mask_clear(&leaf->qs_cpus, cpu);
            atomic_store_rlx(&rcu_global.cpus[cpu].reported_seq, seq);
        }

        struct cpu_mask pending;
        cpu_mask_copy(&pending, &leaf->qs_cpus);
        rcu_propagate_done(leaf, seq, irql);

        for_each_cpu(cpu, &pending) {
            ipi_send((uint32_t) cpu, IRQ_NOP);
        }
    }
}

/* TODO: nonfatal, we'll need to do something a bit smarter here with logging/
 * errors for parseability and whatnot */
static void rcu_report_stall(uint64_t seq, time_ms_t elapsed) {
    struct rcu_stall_blocker blockers[RCU_STALL_MAX_REPORTED];
    size_t nblockers = 0;

    rcu_warn("grace period %lu stalled for %lu ms\n", seq, elapsed);

    for (size_t l = 0; l < rcu_global.leaf_count; l++) {
        struct rcu_node *leaf = &rcu_global.leaves[l];

        struct cpu_mask pending = CPU_MASK_INIT;

        enum irql irql = spin_lock_high(&leaf->lock);

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
                .read_seq = t->rcu_read_seq,
            };
        }

        spin_unlock(&leaf->lock, irql);

        if (!cpu_mask_empty(&pending) || blocked)
            rcu_warn("leaf %zu: cpus %#lx pending, %u blocking reader(s)\n", l,

                     pending.bits[leaf->cpu_base / BITMAP_BITS_PER_WORD],
                     blocked);
    }

    for (size_t i = 0; i < nblockers; i++)
        rcu_warn("tid %zu \"%s\" state %d read_seq %lu\n", blockers[i].id,
                 blockers[i].name ? blockers[i].name : "?", blockers[i].state,
                 blockers[i].read_seq);
}

static uint64_t rcu_gp_start(struct list_head *batch) TSA_NO_ANALYSIS {
    enum irql outer = irql_raise(IRQL_DISPATCH_LEVEL);
    rcu_detach_callbacks(batch);
    atomic_store_rlx(&rcu_global.gp_requests, 0);

    uint64_t prev = atomic_load_rlx(&rcu_global.gp_seq);
    enforce(prev != UINT64_MAX, "RCU GP seq wrap");
    uint64_t seq = prev + 1;

#ifdef TEST_ENABLED
    struct rcu_cb *cb;
    list_for_each_entry(cb, batch, list)
        cb->target_gen = (size_t) seq;
#endif

    /* Root down, rcu.nodes is root then leaves */
    for (size_t i = 0; i < rcu_global.node_count; i++) {
        struct rcu_node *node = &rcu_global.nodes[i];

        enum irql irql = spin_lock_high(&node->lock);
        node->gp_seq = seq;
        node->qs_children = node->full_children;

        if (node->is_leaf) {
            cpu_mask_copy(&node->qs_cpus, &node->full_cpus);
            node->blocked_count = 0;

            /* Everyone here began before the GP */
            struct thread *t;
            list_for_each_entry(t, &node->blocked, rcu_list_node) {
                uint64_t rseq = t->rcu_read_seq;
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

    atomic_store_release(&rcu_global.gp_seq, seq);

    /* Pairs with the xchg in rcu_idle_exit */
    cpu_memory_barrier();

    /* Retire quiescent and poke everyone else */
    cpu_id_t self = smp_id(TOPC_IRQL);

    for (size_t l = 0; l < rcu_global.leaf_count; l++) {
        struct rcu_node *leaf = &rcu_global.leaves[l];

        enum irql irql = spin_lock_high(&leaf->lock);
        if (leaf->gp_seq != seq) {
            spin_unlock(&leaf->lock, irql);
            continue;
        }

        /* Idle CPUs can't be in an RCU read section,
         * and they'd clear the flag before they enter one */
        cpu_id_t cpu;
        for_each_cpu(cpu, &leaf->full_cpus) {
            if (atomic_load_acq(&rcu_global.cpus[cpu].idle))
                cpu_mask_clear(&leaf->qs_cpus, cpu);
        }

        if (leaf == rcu_leaf_for_cpu(self))
            cpu_mask_clear(&leaf->qs_cpus, self);

        struct cpu_mask pending;
        cpu_mask_copy(&pending, &leaf->qs_cpus);
        rcu_propagate_done(leaf, seq, irql);

        for_each_cpu(cpu, &pending)
            ipi_send((uint32_t) cpu, IRQ_NOP);
    }

    irql_lower(outer);
    return seq;
}

static void rcu_gp_wait(uint64_t seq) {
    time_ms_t started = time_get_ms();
    time_ms_t last_kick = started;
    time_ms_t last_stall = started;

    while (atomic_load_acq(&rcu_global.gp_completed) < seq) {
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
    cc_unused(unused_arg);

    while (true) {
        semaphore_wait(&rcu_global.sem);

        if (!rcu_work_pending())
            continue;

        struct list_head batch;
        list_head_init(&batch);

        uint64_t seq = rcu_gp_start(&batch);
        rcu_gp_wait(seq);
        rcu_run_batch(&batch, seq);
    }
}

void rcu_synchronize(void) {
    struct thread *t = thread_get_current();

    enforce(!irq_in_interrupt() && !irq_in_nmi());
    enforce(irql_get() <= IRQL_APC_LEVEL);
    enforce(!t || t->rcu_nesting == 0);
    enforce(!rcu_global.ready || t != rcu_global.worker);

    if (!rcu_global.ready)
        return;

    /* Caller's unpublish happened before this load, meaning
     * grace periods that published a sequence greater than what's
     * here started after the unpublish */
    uint64_t target = atomic_load_acq(&rcu_global.gp_seq) + 1;

    atomic_inc_release(&rcu_global.gp_requests);
    semaphore_post(&rcu_global.sem);

    while (atomic_load_acq(&rcu_global.gp_completed) < target)
        scheduler_yield();
}

static void rcu_build_tree(void) {
    size_t counts[RCU_MAX_LEVELS];
    size_t levels = 0;

    size_t n = global.core_count;
    enforce(n > 0);

    do {
        n = DIV_ROUND_UP(n, RCU_FANOUT);
        enforce(levels < RCU_MAX_LEVELS, "RCU tree deeper than RCU_MAX_LEVELS");
        counts[levels++] = n;
    } while (n > 1);

    /* counts[0] is the leaf, counts[levels - 1] is the root */
    size_t total = 0;
    for (size_t i = 0; i < levels; i++)
        total += counts[i];

    rcu_global.nodes = nofail_vnews(rcu_global.nodes, total, ALLOC_ZERO);
    rcu_global.node_count = total;

    /* Root first so forward pass initializes parents before children */
    size_t offsets[RCU_MAX_LEVELS];
    offsets[levels - 1] = 0;
    for (size_t i = levels - 1; i > 0; i--)
        offsets[i - 1] = offsets[i] + counts[i];

    for (size_t level = 0; level < levels; level++) {
        for (size_t j = 0; j < counts[level]; j++) {
            struct rcu_node *node = &rcu_global.nodes[offsets[level] + j];

            spinlock_init(&node->lock);
            node->gp_seq = 0;
            node->completed_seq = 0;
            node->qs_children = 0;

            if (level == levels - 1) {
                node->parent = NULL;
                node->child_index = 0;
            } else {
                node->parent =
                    &rcu_global.nodes[offsets[level + 1] + (j / RCU_FANOUT)];
                node->child_index = j % RCU_FANOUT;
            }

            size_t owned;
            if (level == 0) {
                node->is_leaf = true;
                node->cpu_base = j * RCU_FANOUT;
                list_head_init(&node->blocked);
                owned = global.core_count - node->cpu_base;
            } else {
                node->is_leaf = false;
                owned = counts[level - 1] - (j * RCU_FANOUT);
            }

            owned = MIN(owned, RCU_FANOUT);

            if (node->is_leaf) {
                cpu_mask_set_range(&node->full_cpus, node->cpu_base, owned);
            } else {
                bitmap_fill(&node->full_children, owned);
            }
        }
    }

    rcu_global.root = &rcu_global.nodes[0];
    rcu_global.leaves = &rcu_global.nodes[offsets[0]];
    rcu_global.leaf_count = counts[0];
}

void rcu_init(void) {
    atomic_store_rlx(&rcu_global.gp_seq, 1);
    atomic_store_rlx(&rcu_global.gp_completed, 1);
    atomic_store_rlx(&rcu_global.gp_requests, 0);

    semaphore_init(&rcu_global.sem, 0, SEMAPHORE_INIT_NORMAL);

    rcu_build_tree();

    rcu_global.cpus =
        nofail_vnews(rcu_global.cpus, global.core_count, ALLOC_ZERO);
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++) {
        spinlock_init(&rcu_global.cpus[cpu].lock);
        list_head_init(&rcu_global.cpus[cpu].list);
        atomic_store_rlx(&rcu_global.cpus[cpu].reported_seq, 0);
    }

    rcu_global.worker = thread_spawn("rcu_gp_worker", rcu_gp_worker);
    rcu_global.ready = true;
}
