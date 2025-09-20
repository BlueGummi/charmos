#include <acpi/lapic.h>
#include <int/idt.h>
#include <smp/smp.h>
#include <sch/apc.h>
#include <sch/sched.h>
#include <types/rcu.h>

#include "internal.h"

/* We will perform scheduler period operations on thread load */

struct scheduler_data scheduler_data = {
    /* This is how many cores can be stealing work at once */
    .max_concurrent_stealers = 0,

    /* This is how many cores are attempting a work steal right now.
     * If this is above the maximum concurrent stealers, we will not
     * attempt any work steals. */
    .active_stealers = 0,

    /* total threads in runqueues of all cores */
    .total_threads = 0,

    /* How much more work the victim must be doing than the stealer
     * for the stealer to go through with the steal. */
    .steal_min_diff = SCHEDULER_DEFAULT_WORK_STEAL_MIN_DIFF,
};

static inline void tick_disable() {
    struct scheduler *self = get_this_core_sched();
    if (atomic_load(&self->tick_enabled)) {
        lapic_timer_disable();
        atomic_store(&self->tick_enabled, false);
    }
}

static inline void tick_enable() {
    struct scheduler *self = get_this_core_sched();
    if (!atomic_load(&self->tick_enabled)) {
        lapic_timer_enable();
        atomic_store(&self->tick_enabled, true);
    }
}

static inline void change_timeslice_duration(uint64_t new_duration) {
    struct scheduler *self = get_this_core_sched();

    /* No need to unnecessarily write to MMIO */
    if (self->timeslice_duration == new_duration &&
        atomic_load(&self->tick_enabled))
        return;

    self->timeslice_duration = new_duration;
    lapic_timer_set_ms(new_duration);
    tick_enable();
}

void scheduler_change_timeslice_duration(uint64_t new_duration) {
    change_timeslice_duration(new_duration);
}

static inline void update_core_current_thread(struct thread *next) {
    struct core *c = global.cores[smp_core_id()];
    c->current_thread = next;
}

static inline void decay_thread_timeslice(struct scheduler *sched,
                                          struct thread *thread) {
    /* If there is no period enabled right now,
     * the thread should not decay timeslices,
     * and we'll just keep it in the ready-queue */
    if (!sched->period_enabled)
        return;

    /* We do not decay non-timesharing thread timeslices */
    if (!THREAD_PRIO_IS_TIMESHARING(thread->perceived_priority))
        return;

    if (thread->timeslices_remaining == 0)
        k_panic("Bug: Thread with no timeslices remaining was saved "
                "to the run queues");

    thread->timeslices_remaining--;

    /* We are done for this period */
    if (thread->timeslices_remaining == 0)
        thread->completed_period = sched->current_period;
}

static inline void update_thread_before_save(struct scheduler *sched,
                                             struct thread *thread,
                                             uint64_t time) {
    thread_set_state(thread, THREAD_STATE_READY);
    thread->curr_core = -1;
    thread_update_runtime_buckets(thread, time);
    decay_thread_timeslice(sched, thread);
}

static inline void do_re_enqueue_thread(struct scheduler *sched,
                                        struct thread *thread) {

    /* Thread just finished an URGENT boost */
    if (thread->perceived_priority == THREAD_PRIO_CLASS_URGENT)
        thread->perceived_priority = thread->base_priority;

    /* Scheduler is locked - called from `schedule()` */
    if (THREAD_PRIO_IS_TIMESHARING(thread->perceived_priority) &&
        thread->timeslices_remaining == 0) {
        scheduler_set_queue_bitmap(sched, thread->perceived_priority);
        retire_thread(sched, thread);
        scheduler_increment_thread_count(sched);
    } else {
        bool locked = true;
        scheduler_add_thread(sched, thread, locked);
    }
}

static inline void update_idle_thread(uint64_t time) {
    struct idle_thread_data *data = smp_core_idle_thread();
    data->did_work_recently = true;
    data->last_exit_ms = time;
}

static inline void update_min_steal_diff(void) {
    atomic_store(&scheduler_data.steal_min_diff,
                 scheduler_compute_steal_threshold());
}

static inline void save_thread(struct scheduler *sched, struct thread *curr,
                               uint64_t time) {
    update_min_steal_diff();

    /* Only save a running thread that exists */
    if (curr && thread_get_state(curr) == THREAD_STATE_RUNNING) {
        update_thread_before_save(sched, curr, time);
        do_re_enqueue_thread(sched, curr);
    } else if (curr && thread_get_state(curr) == THREAD_STATE_IDLE_THREAD) {
        update_idle_thread(time);
    }
}

static inline enum thread_prio_class
available_prio_level_from_bitmap(uint8_t bitmap) {
    return __builtin_ctz(bitmap);
}

static struct thread *pick_from_special_queues(struct scheduler *sched,
                                               enum thread_prio_class prio) {
    struct thread_queue *q = scheduler_get_this_thread_queue(sched, prio);
    struct thread *next = q->head;

    /* Dequeue */
    if (next == q->tail) {
        q->head = NULL;
        q->tail = NULL;
    } else {
        q->head = next->next;
        q->head->prev = q->tail;
        q->tail->next = q->head;
    }

    /* No more threads at this queue level */
    if (q->head == NULL)
        scheduler_clear_queue_bitmap(sched, prio);

    next->next = NULL;
    next->prev = NULL;
    return next;
}

static struct thread *pick_from_regular_queues(struct scheduler *sched,
                                               uint64_t now_ms,
                                               enum thread_prio_class prio) {
    struct thread *next = find_highest_prio(sched, prio);
    if (next)
        return next;

    /* Here, we have been unable to find
     * a thread in the ready queues,
     * so we shall start a new period and swap
     * the pointers and find the
     * thread again */
    swap_queues(sched);
    scheduler_period_start(sched, now_ms);
    return find_highest_prio(sched, prio);
}

static struct thread *pick_thread(struct scheduler *sched, uint64_t now_ms) {
    uint8_t bitmap = scheduler_get_bitmap(sched);
    /* Nothing in queues */
    if (!bitmap)
        return NULL;

    struct thread *next = NULL;

    enum thread_prio_class prio = available_prio_level_from_bitmap(bitmap);
    enum thread_prio_type ptype = prio_type_of(prio);

    if (ptype != THREAD_PRIO_TYPE_TS) {
        next = pick_from_special_queues(sched, prio);
    } else {
        next = pick_from_regular_queues(sched, now_ms, prio);
    }

    scheduler_decrement_thread_count(sched);
    return next;
}

static void load_thread(struct scheduler *sched, struct thread *next,
                        uint64_t time) {
    sched->current = next;
    if (!next)
        return;

    next->curr_core = smp_core_id();
    next->run_start_time = time;
    thread_calculate_activity_data(next);
    thread_classify_activity(next, time);

    /* Do not mark the idle thread as RUNNING because this causes
     * it to enter the runqueues, which is Very Bad™ (it gets enqueued,
     * and becomes treated like a regular thread)! */

    if (thread_get_state(next) != THREAD_STATE_IDLE_THREAD)
        thread_set_state(next, THREAD_STATE_RUNNING);

    update_core_current_thread(next);
}

static inline struct thread *load_idle_thread(struct scheduler *sched) {
    /* Idle thread has no need to have a timeslice
     * No preemption will be occurring since nothing else runs */
    disable_period(sched);
    tick_disable();
    return sched->idle_thread;
}

static void change_timeslice(struct scheduler *sched, struct thread *next) {
    /* Only one thread is running - no timeslice needed */
    if (sched->thread_count == 0) {
        /* Disable the scheduling period because
         * there is no need for period
         * tracking when we have
         * one thread running */
        disable_period(sched);
        tick_disable();
        return;
    }

    if (THREAD_PRIO_HAS_TIMESLICE(next->perceived_priority)) {
        /* Timesharing threads need timeslices */
        change_timeslice_duration(next->timeslice_duration_ms);
    } else {
        /* RT threads do not share time*/
        tick_disable();
    }
}

static inline void context_switch(struct thread *curr, struct thread *next) {
    rcu_mark_quiescent();

    if (curr && curr->state != THREAD_STATE_IDLE_THREAD) {
        switch_context(&curr->regs, &next->regs);
    } else {
        /* Only `load_context` here since nothing was running,
         * typically only used in the very first yield or when
         * exiting the idle thread */
        load_context(&next->regs);
    }
}

void schedule(void) {
    struct scheduler *sched = get_this_core_sched();
    enum irql irql = scheduler_lock(sched);

    uint64_t time = time_get_ms_fast();
    struct thread *curr = sched->current;
    struct thread *next = NULL;

    save_thread(sched, curr, time);

    /* Checks if we can steal, finds a victim, and tries to steal.
     * NULL is returned if any step was unsuccessful */
    struct thread *stolen = scheduler_try_do_steal(sched);

    next = stolen ? stolen : pick_thread(sched, time);

    if (!next) {
        /* Nothing available via steal or in our queues? */
        next = load_idle_thread(sched);
    } else {
        /* Depending on what was loaded, we may or may not
         * need to adjust the timeslice. RT threads do not
         * have timeslices, so the timeslice needs to be
         * disabled if an RT thread is chosen to run */
        change_timeslice(sched, next);
    }

    load_thread(sched, next, time);
    scheduler_unlock(sched, irql);

    context_switch(curr, next);
}

void scheduler_yield() {
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    schedule();
    irql_lower(irql);
    thread_check_and_deliver_apcs(scheduler_get_curr_thread());
}

void scheduler_force_resched(struct scheduler *sched) {
    ipi_send(sched->core_id, IRQ_SCHEDULER);
}
