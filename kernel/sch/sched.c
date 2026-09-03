#include <mem/vmm.h>
#include <sch/periodic_work.h>
#include <sch/sched.h>
#include <smp/smp.h>
#include <sync/rcu.h>
#include <thread/apc.h>
#include <thread/reaper.h>
#include <watchdog.h>

#include "internal.h"

#ifdef DEBUG_LOCK_CHK

#include "sync/lock_chk_internal.h"

static void scheduler_lock_chk_assert(void) {
    lock_chk_assert_schedulable(LOCK_CHK_SITE_HERE());
}

#else

static void scheduler_lock_chk_assert(void) {}

#endif

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

static inline void tick_disable(void) {
    struct scheduler *self = smp_core_scheduler();
    if (scheduler_tick_enabled(self)) {
        timer_delete(&self->tick);
        scheduler_set_tick_enabled(self, false);
    }
}

static inline void tick_enable() {
    struct scheduler *self = smp_core_scheduler();
    if (!scheduler_tick_enabled(self)) {
        timer_modify(&self->tick,
                     timer_delta_us(MS_TO_US(self->tick_duration_ms)));
        scheduler_set_tick_enabled(self, true);
    }
}

void scheduler_tick_enable() {
    tick_enable();
}

void scheduler_tick_disable() {
    tick_disable();
}

static inline void change_tick_duration(time_ms_t new_duration) {
    struct scheduler *self = smp_core_scheduler();

    if (new_duration < 1)
        new_duration = 3;

    if (self->tick_duration_ms != new_duration || !self->tick_enabled) {
        self->tick_duration_ms = new_duration;
        timer_modify(&self->tick, timer_delta_us(MS_TO_US(new_duration)));
        scheduler_set_tick_enabled(self, true);
    }
}

void scheduler_change_tick_duration(uint64_t new_duration) {
    change_tick_duration(new_duration);
}

static inline void update_thread_before_save(struct thread *thread,
                                             time_ms_t time) {
    thread_set_state(thread, THREAD_STATE_READY);
    thread_scale_back_delta(thread);
    thread->curr_core = -1;
    thread_update_runtime_buckets(thread, time);
    thread->virtual_runtime_left = thread_virtual_runtime_left(thread);
}

static inline bool thread_done_for_period(struct thread *thread) {
    return THREAD_PRIO_IS_TIMESHARING(thread->perceived_prio_class) &&
           thread->virtual_budget > 0 &&
           thread->virtual_period_runtime >= thread->virtual_budget;
}

static inline void re_enqueue_thread(struct scheduler *sched,
                                     struct thread *thread) {
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

static inline void update_idle_thread(time_ms_t time) {
    struct idle_thread_data *data = smp_core_idle_thread();
    data->last_exit_ms = time;
}

static inline void update_min_steal_diff(void) {
    atomic_store(&scheduler_data.steal_min_diff,
                 scheduler_compute_steal_threshold());
}

static inline void save_thread(struct scheduler *sched, struct thread *curr,
                               time_ms_t time) {
    update_min_steal_diff();

    /* Only save a running thread that exists */
    if (curr && thread_get_state(curr) == THREAD_STATE_RUNNING) {
        update_thread_before_save(curr, time);
        re_enqueue_thread(sched, curr);
    } else if (curr && thread_get_state(curr) == THREAD_STATE_IDLE_THREAD) {
        update_idle_thread(time);
    }
}

/* returns `true` if the current scheduler lock gets acquired
 * so the caller knows if it needs to acquire it */
static inline bool migrate_to_destination(struct thread *t, time_ms_t time) {
    int64_t dst;
    if (!t || (dst = thread_set_migration_target(t, -1)) == -1)
        return false;

    if (dst == (int64_t) smp_core_id())
        return false;

    enum irql irql_us, irql_other;
    struct scheduler *us = smp_core_scheduler();
    struct scheduler *other = global.schedulers[dst];

    /* They're both DISPATCH */
    scheduler_acquire_two_locks(us, other, &irql_us, &irql_other);

    /* mark our own other_locked as `other` so that
     * upon the switch-in, the lock is dropped */
    us->other_locked = other;

    /* save ourselves to the other scheduler */
    save_thread(other, t, time);
    thread_set_runqueue(t, other);

    thread_post_migrate(t, us->core_id, dst);
    return true;
}

static struct thread *pick_from_special_queues(struct scheduler *sched,
                                               enum thread_prio_class prio) {
    struct list_head *q = scheduler_get_this_thread_queue(sched, prio);
    struct list_head *node = list_pop_front_init(q);
    kassert(node);

    return thread_from_rq_list_node(node);
}

static struct thread *pick_from_regular_queues(struct scheduler *sched,
                                               time_ms_t now_ms) {
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

static struct thread *pick_thread(struct scheduler *sched, time_ms_t now_ms) {
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
                        time_ms_t time) {
    sched->current = next;
    smp_core()->current_thread = next;

    kassert(next);

    /* Do not mark the idle thread as RUNNING because this causes
     * it to enter the runqueues, which is Very Bad™ (it gets enqueued,
     * and becomes treated like a regular thread)! */
    if (next->state != THREAD_STATE_IDLE_THREAD)
        thread_set_state(next, THREAD_STATE_RUNNING);

    thread_set_runqueue(next, sched);
    next->curr_core = smp_core_id();
    next->run_start_time = time;

    thread_calculate_activity_data(next);
    thread_classify_activity(next, time);
}

static inline struct thread *load_idle_thread(struct scheduler *sched) {

    /* Idle thread has no need to have a tick
     * No preemption will be occurring since nothing else runs */
    tick_disable();
    disable_period(sched);

    struct idle_thread_data *idle = smp_core_idle_thread();

    atomic_store(&idle->last_entry_ms, time_get_ms());

    scheduler_mark_self_idle(true);

    return sched->idle_thread;
}

static void change_tick(struct scheduler *sched, struct thread *next) {
    /* Only one thread is running - no timeslice needed */
    if (sched->total_thread_count == 0 && sched->completed_rbt.root == NULL) {
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
    } else if (next->perceived_prio_class == THREAD_PRIO_CLASS_RT) {
        /* Realtime threads get the tick disabled. Only RT though.
         * URGENT still needs it so that it can switch out and
         * run another thread when the boost leaves */
        tick_disable();
    }
}

static inline void context_switch(struct thread *curr, struct thread *next) {
    if (curr)
        thread_or_flags(curr, THREAD_FLAG_YIELDED);

    if (curr != next)
        next->context_switches++;

    /* We are responsible for dropping references
     * on threads entering their last yield */
    bool just_load = false;

    if (!curr)
        just_load = true;

    if (curr && curr->state == THREAD_STATE_IDLE_THREAD)
        just_load = true;

    if (unlikely(curr && curr->state == THREAD_STATE_ZOMBIE)) {
        just_load = true;
        kassert(!smp_core_scheduler()->drop_last_ref);
        smp_core_scheduler()->drop_last_ref = curr;
    }

    if (just_load) {
        load_context(&next->regs);
    } else {
        switch_context(&curr->regs, &next->regs);
    }
}

void schedule(void) {
    time_ms_t time = time_get_ms();

    struct scheduler *sched = smp_core_scheduler();

    struct thread *curr = sched->current;
    struct thread *next = NULL;

    /* if this returns false, the thread was not migrated
     * anywhere and we're responsible for acquiring our lock
     * and also saving it to our runqueues. if it returns true,
     * both our lock and the other CPU's locks for schedulers
     * are acquired (ordered by memory address) and we don't
     * have to acquire it or save the thread to our CPU since
     * that happened in migrate_to_destination */
    if (!migrate_to_destination(curr, time)) {

        /* We have to disable interrupts here: why? well,
         * because if we don't, we can get an IRQ right now,
         * and in that ISR we can attempt to acquire this lock,
         * and then we are in a big pickle since we deadlock */
        enum irql irql = spin_lock_irq_disable(&sched->lock);
        (void) irql;

        save_thread(sched, curr, time);
    }

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

    /* The last known good point before next becomes current
     *
     * Threads still in a read-side critical section
     * are registered here */
    rcu_note_context_switch(curr, /* next_is_idle = */ next->state ==
                                      THREAD_STATE_IDLE_THREAD);

    load_thread(sched, next, time);

    context_switch(curr, next);
}

void scheduler_switch_in() {
    struct scheduler *us = smp_core_scheduler();
    struct scheduler *other = us->other_locked;
    us->other_locked = NULL;

    kassert(us != other);

    if (!other) {
        /* guaranteed to be the last IRQL, we always
         * raise there, so it can't be any other */
        spin_unlock(&us->lock, IRQL_DISPATCH_LEVEL);
    } else {
        scheduler_release_two_locks(us, other, IRQL_DISPATCH_LEVEL,
                                    IRQL_DISPATCH_LEVEL);
    }

    struct thread *drop = us->drop_last_ref;
    us->drop_last_ref = NULL;
    if (drop)
        thread_put(drop);

    scheduler_periodic_work_execute(PERIODIC_WORK_PERIOD_BASED);
    vmm_reclaim_page_tables();
    atomic_store(&smp_core()->pt_seen_epoch, atomic_load(&global.pt_epoch));
    watchdog_pet();
}

/* Looping here to prevent unbounded recursion, keeping the max nesting == 1
 * invariant upheld. Interrupt state stays uniform across
 * iterations, and in_resched is cleared before the lower so DPCs
 * have a chance to run */
static void scheduler_yield_loop(void) {
    do {
        enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
        scheduler_mark_self_in_resched(true);

        schedule();

        scheduler_switch_in();

        scheduler_mark_self_in_resched(false);

        irql_lower_no_resched(irql);
    } while (scheduler_mark_self_needs_resched(false));
}

void scheduler_yield() {

    /* Read GS without interrupts on, a temporary HACK: */
    bool iflag = are_interrupts_enabled();
    disable_interrupts();

    struct core *c = smp_core();
    bool entry_in_resched = atomic_load(&c->in_resched);
    uint32_t entry_depth = ctx_preempt_count(c->ctx);
    cpu_id_t entry_cpu = c->id;

    if (iflag)
        enable_interrupts();

    kassert(!entry_in_resched, "yielding while already in resched on cpu %zu",
            (size_t) entry_cpu);
    kassert(entry_depth == 0, "yielding on cpu %zu with preempt depth %u",
            (size_t) entry_cpu, entry_depth);

    scheduler_yield_nesting_enter(thread_get_current());

    scheduler_lock_chk_assert();
    scheduler_yield_loop();

    /* It did the matching enter already, so we exit here */
    scheduler_yield_nesting_exit(thread_get_current());
}
