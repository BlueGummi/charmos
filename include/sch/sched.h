/* @title: Scheduler */
#pragma once
#include <global.h>
#include <irq/ipi.h>
#include <sch/domain.h>
#include <smp/core.h>
#include <smp/topology.h>
#include <stdarg.h>
#include <stdbool.h>
#include <structures/rbt.h>
#include <sync/spinlock.h>
#include <thread/thread_types.h>
#include <time/timer.h>
#include <tsa.h>

#define WORK_STEAL_THRESHOLD                                                   \
    75ULL /* How little work the core needs to be                              \
           * doing to try to steal work from another                           \
           * core. This means "% of the average"                               \
           */

#define SCHEDULER_DEFAULT_WORK_STEAL_MIN_DIFF 130

struct idle_thread_data {
    atomic_uint64_t last_entry_ms;
    uint64_t last_exit_ms;
};

struct scheduler {
    /* Current tick data */
    atomic_bool tick_enabled;
    time_ms_t tick_duration_ms;

    /* Structures */
    struct list_head urgent_threads;

    struct rbt thread_rbt;
    struct rbt completed_rbt;

    struct list_head rt_threads;
    struct list_head bg_threads;

    struct rbt climb_threads; /* threads on this CPU participating in CLIMB */

    atomic_uint8_t queue_bitmap;

    struct thread *current;
    struct thread *drop_last_ref;
    struct timer tick;

    struct wroost_percpu_data *wroost_data;

    /* Thread count at each prio */
    size_t thread_count[THREAD_PRIO_CLASS_COUNT];
    size_t total_thread_count;
    size_t total_weight;

    /* Period information */
    bool period_enabled;
    uint64_t current_period;

    time_ms_t period_ms;
    time_ms_t period_start_ms; /* Timestamp */

#ifdef PROFILING_SCHED
    size_t periods_started; /* How many have we started?
                             * (Each one must complete) */

    size_t idle_thread_loads;
#endif

    cpu_id_t core_id;

    /* TODO: Rework time load balancing away from this foolery */

    /* Work steal/migration */
    atomic_bool being_robbed;
    atomic_bool stealing_work;

    struct spinlock lock;

    /* Idle thread data */
    struct thread *idle_thread;
    struct idle_thread_data idle_thread_data;

    struct scheduler *other_locked; /* If we acquired the lock of another
                                     * scheduler in scheduler_yield(),
                                     * we store a pointer to it here.
                                     *
                                     * If this is NULL, we didn't do that,
                                     * but in the case that it isn't, we must
                                     * drop the raw lock for this in addition
                                     * to the lock for our scheduler */

    enum irql locked_irql;
    enum irql other_locked_irql;

    struct cpu_mask deferred_resched_mask;
    struct rt_scheduler_percpu *rt;

#ifdef DEBUG_SCHED
    /* Runqueue progress, racily read by detector */
    atomic_uint64_t debug_progress_ms;
#endif
};

void scheduler_init();

#ifdef DEBUG_SCHED
void scheduler_debug_start(void);
#else
static inline void scheduler_debug_start(void) {}
#endif

struct scheduler *scheduler_select_best_for_thread(struct thread *t);
void scheduler_add_thread_locked(struct scheduler *sched, struct thread *thread)
    TSA_CALLER_HOLDS(&sched->lock);

void scheduler_add_thread(struct scheduler *sched, struct thread *thread);

void scheduler_remove_thread_locked(struct scheduler *sched, struct thread *t)
    TSA_CALLER_HOLDS(&sched->lock);

void scheduler_remove_thread(struct scheduler *sched, struct thread *t);
void schedule(void);
void k_sch_main(void *);
void scheduler_idle_main(void *);
void scheduler_yield(void);

void scheduler_period_start(struct scheduler *s, uint64_t now_ms);

void switch_context(struct cpu_context *old, struct cpu_context *new);
void load_context(struct cpu_context *new);
void save_context(struct cpu_context *new);

bool scheduler_can_steal_work(struct scheduler *sched);
bool scheduler_can_take_thread(size_t core, struct thread *target);
uint64_t scheduler_compute_steal_threshold();
struct thread *scheduler_try_do_steal(struct scheduler *sched);

struct scheduler *scheduler_pick_victim(struct scheduler *self);
struct thread *scheduler_steal_work(struct scheduler *new,
                                    struct scheduler *victim);

size_t scheduler_try_push_to_idle_core(struct scheduler *sched);

void scheduler_tick_enable();
void scheduler_tick_disable();
enum irq_result scheduler_reschedule_isr(struct irq_context *rsp, void *ctx);

/* For a global structure containing central scheduler data */
struct scheduler_data {
    uint32_t max_concurrent_stealers;
    atomic_uint32_t active_stealers;
    atomic_uint32_t total_threads;
    atomic_int64_t steal_min_diff;
};

extern struct scheduler_data scheduler_data;

static inline bool scheduler_self_in_resched() {
    return smp_read(TOPC_NONE, in_resched);
}

/* Because this is always executed at or above DISPATCH it is fine */
static inline bool scheduler_mark_self_in_resched(bool new) {
    return atomic_exchange(&smp_core(TOPC_IRQL)->in_resched, new);
}

static inline bool scheduler_mark_core_needs_resched(struct core *c, bool new) {
    return atomic_exchange(&c->needs_resched, new);
}

static inline bool scheduler_mark_self_needs_resched(bool new) {
    return scheduler_mark_core_needs_resched(smp_core(TOPC_NONE), new);
}

static inline bool scheduler_mark_core_needs_run_dpcs(struct core *c,
                                                      bool new) {
    return atomic_exchange(&c->needs_run_dpcs, new);
}

static inline bool scheduler_mark_self_needs_run_dpcs(bool new) {
    return scheduler_mark_core_needs_run_dpcs(smp_core(TOPC_NONE), new);
}

/* this is only ever called when a thread is loaded */
static inline void scheduler_mark_self_idle(bool new) {
    /* the old value is different from the new one */
    struct core *c = smp_core(TOPC_IRQL); /* Only ever called from
                                           * the reschedule routine */

    if (atomic_load_rlx(&c->idle) != new) {
        atomic_store_rlx(&c->idle, new);
        topology_mark_core_idle(c->id, new);
        scheduler_domain_mark_self_idle(new);
        if (new) {
            atomic_inc_acq_rel(&global.idle_core_count);
        } else {
            atomic_dec_acq_rel(&global.idle_core_count);
        }
    }
}

static inline void scheduler_resched_if_needed(void) {
    if (scheduler_self_in_resched())
        return;

    if (scheduler_mark_self_needs_resched(false))
        scheduler_yield();
}

static inline bool scheduler_core_idle(struct core *c) {
    return atomic_load(&c->idle);
}

/* NOTE: this function is unsafe to call from context that holds a runqueue
 * lock. The reason:
 *
 * Imagine the following:
 *     CPU 0 is holding RQ 0's lock
 *     CPU 0 IPI's CPU 1
 *     CPU 1, before that IPI arrives, was spinning on
 *            RQ 0's lock, since it was migrating a thread.
 *
 *            Thus, CPU 1 cannot respond to the IPI
 *            because it has interrupts disabled,
 *            and will never enter the ISR to ack.
 *
 *     CPU 0 spins on waiting for this (uncompletable) IPI to complete
 *     CPU 1 is also stuck waiting for RQ 0's lock to be relinquished,
 *           which is something that won't happen because of this
 *           dependency
 *
 *     *boom* deadlock.
 *
 *     scheduler core code is supposed to call scheduler_defer_resched()
 *     defined in the internal header there.
 */
static inline void scheduler_force_resched(struct scheduler *sched) {
    scheduler_mark_core_needs_resched(global.cores[sched->core_id], true);
    ipi_send(sched->core_id, IRQ_SCHEDULER);
}

static inline bool cc_warn_unused_result
scheduler_try_force_resched(struct scheduler *sched) {
    scheduler_mark_core_needs_resched(global.cores[sched->core_id], true);
    return ipi_send_try(sched->core_id, IRQ_SCHEDULER);
}

enum irql thread_lock_scheduler(struct thread *t, struct scheduler **out_sched)
    TSA_ACQUIRES_SPIN(&(*out_sched)->lock);

/* Uses the blocking ipi_send(), so no spinlock may be held. */
static inline void scheduler_force_run_dpcs(cpu_id_t cpu) {
    ipi_send(cpu, IRQ_DPC);
}

static inline bool scheduler_preemption_disabled(enum topology_caller c) {
    return (smp_ctx(c) & SMP_CTX_PREEMPT_MASK) != 0;
}
