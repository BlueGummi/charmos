/* @title: Scheduler */
#pragma once
#include <acpi/lapic.h>
#include <global.h>
#include <sch/domain.h>
#include <smp/core.h>
#include <smp/topology.h>
#include <stdarg.h>
#include <stdbool.h>
#include <structures/rbt.h>
#include <sync/spinlock.h>
#include <thread/thread_types.h>

#define WORK_STEAL_THRESHOLD                                                   \
    75ULL /* How little work the core needs to be                              \
           * doing to try to steal work from another                           \
           * core. This means "% of the average"                               \
           */

#define SCHEDULER_DEFAULT_WORK_STEAL_MIN_DIFF 130

struct idle_thread_data {
    _Atomic uint64_t last_entry_ms;
    uint64_t last_exit_ms;
};

struct scheduler {
    /* Current tick data */
    atomic_bool tick_enabled;
    time_t tick_duration_ms;

    /* Structures */
    struct list_head urgent_threads;

    struct rbt thread_rbt;
    struct rbt completed_rbt;

    struct list_head rt_threads;
    struct list_head bg_threads;

    _Atomic uint8_t queue_bitmap;

    struct thread *current;

    /* Thread count at each prio */
    size_t thread_count[THREAD_PRIO_CLASS_COUNT];
    size_t total_thread_count;
    size_t total_weight;

    /* Period information */
    bool period_enabled;
    uint64_t current_period;

    time_t period_ms;
    time_t period_start_ms; /* Timestamp */

#ifdef PROFILING_SCHED
    size_t periods_started; /* How many have we started?
                             * (Each one must complete) */

    size_t idle_thread_loads;
#endif

    uint64_t core_id;

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
};

void scheduler_init();

void scheduler_add_thread(struct scheduler *sched, struct thread *thread,
                          bool lock_held);
void scheduler_remove_thread(struct scheduler *sched, struct thread *t,
                             bool lock_held);
void schedule(void);
void k_sch_main(void *);
void scheduler_idle_main(void *);
void scheduler_yield();
void scheduler_enqueue(struct thread *t);
void scheduler_enqueue_on_core(struct thread *t, uint64_t core_id);

bool scheduler_wake(struct thread *t, enum thread_wake_reason reason,
                    enum thread_prio_class prio, void *wake_src);

void scheduler_period_start(struct scheduler *s, uint64_t now_ms);

void switch_context(struct cpu_context *old, struct cpu_context *new);
void load_context(struct cpu_context *new);
void save_context(struct cpu_context *new);

bool scheduler_can_steal_work(struct scheduler *sched);
bool scheduler_can_steal_thread(size_t core, struct thread *target);
uint64_t scheduler_compute_steal_threshold();
struct thread *scheduler_try_do_steal(struct scheduler *sched);

struct scheduler *scheduler_pick_victim(struct scheduler *self);
struct thread *scheduler_steal_work(struct scheduler *victim);
size_t scheduler_try_push_to_idle_core(struct scheduler *sched);
bool scheduler_inherit_priority(struct thread *boosted, size_t new_weight,
                                enum thread_prio_class new_class,
                                size_t *old_weight_out,
                                enum thread_prio_class *old_class_out);
void scheduler_uninherit_priority(size_t weight, enum thread_prio_class class);
void scheduler_tick_enable();
void scheduler_tick_disable();
enum irq_result scheduler_timer_isr(void *ctx, uint8_t vector,
                                    struct irq_context *rsp);

/* For a global structure containing central scheduler data */
struct scheduler_data {
    uint32_t max_concurrent_stealers;
    _Atomic uint32_t active_stealers;
    _Atomic uint32_t total_threads;
    _Atomic int64_t steal_min_diff;
};

extern struct scheduler_data scheduler_data;

static inline struct thread *scheduler_get_current_thread() {
    uintptr_t thread;
    asm volatile("movq %%gs:%c1, %0"
                 : "=r"(thread)
                 : "i"(offsetof(struct core, current_thread)));
    return (struct thread *) thread;
}

static inline struct thread *thread_spawn(char *name, void (*entry)(void *),
                                          void *arg, ...) {
    va_list args;
    va_start(args, arg);
    struct thread *t =
        thread_create_internal(name, entry, arg, THREAD_STACK_SIZE, args);
    va_end(args);
    scheduler_enqueue(t);
    return t;
}

static inline struct thread *thread_spawn_custom_stack(char *name,
                                                       void (*entry)(void *),
                                                       void *arg,
                                                       size_t stack_size, ...) {
    va_list args;
    va_start(args, stack_size);
    struct thread *t =
        thread_create_internal(name, entry, arg, stack_size, args);
    va_end(args);

    scheduler_enqueue(t);
    return t;
}

static inline struct thread *thread_spawn_on_core(char *name,
                                                  void (*entry)(void *),
                                                  void *arg, uint64_t core_id,
                                                  ...) {
    va_list args;
    va_start(args, core_id);
    struct thread *t =
        thread_create_internal(name, entry, arg, THREAD_STACK_SIZE, args);
    va_end(args);

    scheduler_enqueue_on_core(t, core_id);
    return t;
}

void scheduler_wake_from_io_block(struct thread *t, void *wake_src);


static inline bool scheduler_self_in_resched() {
    return atomic_load(&smp_core()->in_resched);
}

static inline bool scheduler_mark_self_in_resched(bool new) {
    return atomic_exchange(&smp_core()->in_resched, new);
}

#define TICKS_FOR_PRIO(level) (level == THREAD_PRIO_LOW ? 64 : 1ULL << level)

static inline bool scheduler_mark_core_needs_resched(struct core *c, bool new) {
    return atomic_exchange(&c->needs_resched, new);
}

static inline bool scheduler_mark_self_needs_resched(bool new) {
    return scheduler_mark_core_needs_resched(smp_core(), new);
}

static inline bool scheduler_self_needs_resched(void) {
    return atomic_load(&smp_core()->needs_resched);
}

/* this is only ever called when a thread is loaded */
static inline void scheduler_mark_self_idle(bool new) {
    /* the old value is different from the new one */
    struct core *c = smp_core();

    if (c->idle != new) {
        c->idle = new;
        topology_mark_core_idle(c->id, new);
        scheduler_domain_mark_self_idle(new);
        if (new) {
            atomic_fetch_add_explicit(&global.idle_core_count, 1,
                                      memory_order_acq_rel);
        } else {
            atomic_fetch_sub_explicit(&global.idle_core_count, 1,
                                      memory_order_acq_rel);
        }

        /* set the DPC event. once we exit the yield(),
         * we will run DPCs that correspond to the status of
         * IDLE/WOKE, and then unset the status */
        c->dpc_event = new ? DPC_CPU_IDLE : DPC_CPU_WOKE;
    }
}

static inline void scheduler_resched_if_needed(void) {
    if (scheduler_self_in_resched())
        return;

    if (scheduler_mark_self_needs_resched(false)) {
        scheduler_yield();
    }
}

static inline bool scheduler_core_idle(struct core *c) {
    return atomic_load(&c->idle);
}

static inline void scheduler_force_resched(struct scheduler *sched) {
    if (sched->core_id == smp_core_id()) {
        scheduler_mark_self_needs_resched(true);
    } else {
        struct core *other = global.cores[sched->core_id];
        if (!other) {
            ipi_send(sched->core_id, IRQ_SCHEDULER);
            return;
        }

        scheduler_mark_core_needs_resched(other, true);
        ipi_send(sched->core_id, IRQ_SCHEDULER);
    }
}

static inline uint32_t scheduler_preemption_disable(void) {
    struct core *cpu = smp_core();

    uint32_t old =
        atomic_fetch_add(&cpu->scheduler_preemption_disable_depth, 1);

    if (old == UINT32_MAX) {
        k_panic("overflow\n");
    }

    return old + 1;
}

static inline uint32_t scheduler_preemption_enable(void) {
    struct core *cpu = smp_core();

    uint32_t old =
        atomic_fetch_sub(&cpu->scheduler_preemption_disable_depth, 1);

    if (old == 0) {
        k_panic("underflow\n");
    }

    return old - 1;
}

static inline bool scheduler_preemption_disabled(void) {
    return atomic_load(&smp_core()->scheduler_preemption_disable_depth) > 0;
}
