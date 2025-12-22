#include <acpi/lapic.h>
#include <sch/sched.h>
#include <smp/smp.h>
#include <sync/rcu.h>
#include <thread/apc.h>

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
    struct scheduler *self = smp_core_scheduler();
    if (scheduler_tick_enabled(self)) {
        lapic_timer_disable();
        scheduler_set_tick_enabled(self, false);
    }
}

static inline void tick_enable() {
    struct scheduler *self = smp_core_scheduler();
    if (!scheduler_tick_enabled(self)) {
        lapic_timer_enable();
        scheduler_set_tick_enabled(self, true);
    }
}

void scheduler_tick_enable() {
    tick_enable();
}

void scheduler_tick_disable() {
    tick_disable();
}

static inline void change_tick_duration(uint64_t new_duration) {
    struct scheduler *self = smp_core_scheduler();

    /* Tick duration is the same */
    if (self->tick_duration_ms == new_duration && scheduler_tick_enabled(self))
        return;

    self->tick_duration_ms = new_duration;
    lapic_timer_set_ms(new_duration);
    tick_enable();
}

void scheduler_change_tick_duration(uint64_t new_duration) {
    change_tick_duration(new_duration);
}

static inline bool needs_migration(struct thread *t) {
    return t->migrate_to != -1;
}

static inline void update_thread_before_save(struct thread *thread,
                                             time_t time) {
    thread_set_state(thread, THREAD_STATE_READY);
    thread_scale_back_delta(thread);
    thread->curr_core = -1;
    thread_update_runtime_buckets(thread, time);
    thread->virtual_runtime_left = thread_virtual_runtime_left(thread);
}

static inline bool thread_done_for_period(struct thread *thread) {
    return THREAD_PRIO_IS_TIMESHARING(thread->perceived_prio_class) &&
           thread->virtual_period_runtime >= thread->virtual_budget;
}

static inline void re_enqueue_thread(struct scheduler *sched,
                                     struct thread *thread) {
    /* Thread just finished an URGENT boost */
    if (thread->perceived_prio_class == THREAD_PRIO_CLASS_URGENT)
        thread->perceived_prio_class = thread->base_prio_class;

    /* Scheduler is locked - called from `schedule()` */
    if (thread_done_for_period(thread)) {
        thread->virtual_runtime_left = thread->virtual_budget;
        thread->completed_period = sched->current_period;
        retire_thread(sched, thread);
        scheduler_increment_thread_count(sched, thread);
    } else {
        bool locked = true;
        scheduler_add_thread(sched, thread, locked);
    }
}

/* TODO: this!!! */
static inline void migrate_to_destination(struct thread *t) {
    struct scheduler *dst = global.schedulers[t->migrate_to];

    enum irql irql = scheduler_lock_irq_disable(dst);

    scheduler_unlock(dst, irql);
}

static inline void update_idle_thread(time_t time) {
    struct idle_thread_data *data = smp_core_idle_thread();
    data->last_exit_ms = time;
}

static inline void update_min_steal_diff(void) {
    atomic_store(&scheduler_data.steal_min_diff,
                 scheduler_compute_steal_threshold());
}

static inline void save_thread(struct scheduler *sched, struct thread *curr,
                               time_t time) {
    update_min_steal_diff();

    /* Only save a running thread that exists */
    if (curr && thread_get_state(curr) == THREAD_STATE_RUNNING) {
        update_thread_before_save(curr, time);
        re_enqueue_thread(sched, curr);
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
    struct list_head *q = scheduler_get_this_thread_queue(sched, prio);
    struct list_head *node = list_pop_front_init(q);
    kassert(node);

    return thread_from_rq_list_node(node);
}

static struct thread *pick_from_regular_queues(struct scheduler *sched,
                                               time_t now_ms) {
    struct thread *next = find_highest_prio(sched);
    if (next)
        return next;

    /* Here, we have been unable to find
     * a thread in the ready queues,
     * so we shall start a new period and swap
     * the pointers and find the thread again */
    swap_queues(sched);
    scheduler_period_start(sched, now_ms);
    return find_highest_prio(sched);
}

static struct thread *pick_thread(struct scheduler *sched, time_t now_ms) {
    uint8_t bitmap = scheduler_get_bitmap(sched);
    /* Nothing in queues */
    if (!bitmap)
        return NULL;

    struct thread *next = NULL;

    enum thread_prio_class prio = available_prio_level_from_bitmap(bitmap);

    if (prio != THREAD_PRIO_CLASS_TIMESHARE) {
        next = pick_from_special_queues(sched, prio);
    } else {
        next = pick_from_regular_queues(sched, now_ms);
    }

    kassert(next); /* cannot be NULL - if it is the bitmap is lying */
    scheduler_decrement_thread_count(sched, next);

    /* make sure we are not idle */
    scheduler_mark_self_idle(false);

    return next;
}

static void load_thread(struct scheduler *sched, struct thread *next,
                        time_t time) {
    sched->current = next;
    smp_core()->current_thread = next;

    kassert(next);

    thread_set_last_ran(next, smp_core_id());
    next->curr_core = smp_core_id();
    next->run_start_time = time;
    spin_unlock_raw(&next->being_moved);
    thread_calculate_activity_data(next);
    thread_classify_activity(next, time);

    /* Do not mark the idle thread as RUNNING because this causes
     * it to enter the runqueues, which is Very Bad™ (it gets enqueued,
     * and becomes treated like a regular thread)! */
    if (next->state != THREAD_STATE_IDLE_THREAD)
        thread_set_state(next, THREAD_STATE_RUNNING);
}

static inline struct thread *load_idle_thread(struct scheduler *sched) {

    /* Idle thread has no need to have a tick
     * No preemption will be occurring since nothing else runs */
    tick_disable();
    disable_period(sched);

    struct idle_thread_data *idle = smp_core_idle_thread();

    atomic_store(&idle->woken_from_timer, false);
    atomic_store(&idle->last_entry_ms, time_get_ms());
    atomic_store(&idle->state, IDLE_THREAD_WORK_STEAL);

    scheduler_mark_self_idle(true);

    return sched->idle_thread;
}

static void change_tick(struct scheduler *sched, struct thread *next) {
    /* Only one thread is running - no timeslice needed */
    if (sched->total_thread_count == 0) {
        /* Disable the scheduling period because
         * there is no need for period
         * tracking when we have
         * one thread running */
        disable_period(sched);
        tick_disable();
        return;
    }

    if (THREAD_PRIO_HAS_TIMESLICE(next->perceived_prio_class) &&
        thread_get_state(next) != THREAD_STATE_IDLE_THREAD) {
        /* Timesharing threads need timeslices */
        change_tick_duration(next->timeslice_length_raw_ms);
    } else {
        tick_disable();
    }
}

static inline void context_switch(struct scheduler *sched, struct thread *curr,
                                  struct thread *next) {
    if (curr)
        atomic_store_explicit(&curr->yielded_after_wait, true,
                              memory_order_release);

    if (curr != next)
        next->context_switches++;

    /* We are responsible for dropping references
     * on threads entering their last yield */
    bool just_load = false;

    if (!curr)
        just_load = true;

    if (unlikely(curr && curr->state == THREAD_STATE_ZOMBIE)) {
        just_load = true;
        thread_put(curr);
    }

    if (curr && curr->state == THREAD_STATE_IDLE_THREAD) {
        just_load = true;
    }

    sched->switched_out = just_load ? NULL : curr;

    if (curr == next)
        return;

    if (!just_load) {
        switch_context(&curr->regs, &next->regs);
    } else {
        load_context(&next->regs);
    }
}

void schedule(void) {
    time_t time = time_get_ms();

    struct scheduler *sched = smp_core_scheduler();

    struct thread *curr = sched->current;
    struct thread *next = NULL;

    spin_lock_raw(&sched->lock);

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
        change_tick(sched, next);
    }

    load_thread(sched, next, time);

    context_switch(sched, curr, next);
}

void scheduler_drop_locks_after_switch_in() {
    spin_unlock_raw(&smp_core_scheduler()->lock);
}

void scheduler_yield() {
    kassert(!scheduler_self_in_resched());

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    scheduler_mark_self_in_resched(true);

    schedule();

    scheduler_drop_locks_after_switch_in();

    scheduler_mark_self_in_resched(false);
    irql_lower(irql);
}
