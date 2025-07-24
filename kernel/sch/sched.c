#include <acpi/lapic.h>
#include <asm.h>
#include <boot/stage.h>
#include <compiler.h>
#include <console/printf.h>
#include <int/idt.h>
#include <mp/mp.h>
#include <registry.h>
#include <sch/sched.h>
#include <sleep.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <tests.h>

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

static inline void disable_timeslice() {
    if (lapic_timer_is_enabled())
        lapic_timer_disable();
}

static inline void enable_timeslice() {
    if (!lapic_timer_is_enabled())
        lapic_timer_enable();
}

void scheduler_enable_timeslice() {
    enable_timeslice();
}

void scheduler_disable_timeslice() {
    disable_timeslice();
}

static inline void update_core_current_thread(struct thread *next) {
    struct core *c = global.cores[get_this_core_id()];
    c->current_thread = next;
}

static inline void maybe_recompute_steal_threshold(uint64_t core_id) {
    if (core_id == 0)
        scheduler_data.steal_min_diff = scheduler_compute_steal_threshold();
}

/* Higher number == lower priority */
static inline enum thread_priority get_decayed_prio(enum thread_priority curr) {
    return curr == THREAD_PRIO_LOW ? THREAD_PRIO_HIGH : curr + 1;
}

static inline void do_thread_prio_decay(struct thread *thread) {
    /* Nothing happens - RT threads do not decay */
    if (thread->perceived_prio == THREAD_PRIO_RT)
        return;

    /* Reset the time for the new priority */
    thread->time_in_level = 0;

    /* Timesharing threads decay and then get re-boosted to THREAD_PRIO_HIGH */
    if (THREAD_PRIO_IS_TIMESHARING(thread->perceived_prio))
        thread->perceived_prio = get_decayed_prio(thread->perceived_prio);

    /* Reset the priority to the base priority. URGENT is only set from
     * explicit boosts for device interrupts, and we must reset it here. */
    if (thread->perceived_prio == THREAD_PRIO_URGENT)
        thread->perceived_prio = thread->base_prio;
}

static inline void update_thread_before_save(struct thread *thread) {
    atomic_store(&thread->state, THREAD_STATE_READY);
    thread->curr_core = -1;
    thread->time_in_level++;

    /* Decay the priority depending on what the thread class is */
    if (thread->time_in_level >= TICKS_FOR_PRIO(thread->perceived_prio))
        do_thread_prio_decay(thread);
}

static inline void do_re_enqueue_thread(struct scheduler *sched,
                                        struct thread *thread) {
    /* Scheduler is locked - called from `schedule()` */
    bool locked = true;
    scheduler_add_thread(sched, thread, locked);
}

static inline void do_save_thread(struct scheduler *sched,
                                  struct thread *curr) {
    update_thread_before_save(curr);
    do_re_enqueue_thread(sched, curr);
}

static inline void update_idle_thread(void) {
    struct idle_thread_data *data = get_this_core_idle_thread();
    data->did_work_recently = true;
    data->last_exit_ms = time_get_ms();
}

static inline void requeue_current_thread_if_runnable(struct scheduler *sched,
                                                      struct thread *curr) {

    /* Core 0 will recompute the steal threshold */
    maybe_recompute_steal_threshold(get_this_core_id());

    /* Only save a running thread that exists */
    if (curr && atomic_load(&curr->state) == THREAD_STATE_RUNNING) {
        do_save_thread(sched, curr);
    } else if (curr && atomic_load(&curr->state) == THREAD_STATE_IDLE_THREAD) {
        update_idle_thread();
    }
}

static struct thread *scheduler_pick_regular_thread(struct scheduler *sched) {
    uint8_t bitmap = atomic_load(&sched->queue_bitmap);

    /* Nothing in queues */
    if (!bitmap)
        return NULL;

    int lvl = __builtin_ctz(bitmap);
    bitmap &= ~(1 << lvl);

    struct thread_queue *q = &sched->queues[lvl];
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
        sched->queue_bitmap &= ~(1 << lvl);

    next->next = NULL;
    next->prev = NULL;

    scheduler_decrement_thread_count(sched);

    return next;
}

static void load_thread(struct scheduler *sched, struct thread *next) {
    sched->current = next;
    if (!next)
        return;

    next->curr_core = get_this_core_id();

    /* Do not mark the idle thread as RUNNING because this causes
     * it to enter the runqueues, which is Very Bad™ (it gets enqueued,
     * and becomes treated like a regular thread)! */

    if (atomic_load(&next->state) != THREAD_STATE_IDLE_THREAD)
        atomic_store(&next->state, THREAD_STATE_RUNNING);

    update_core_current_thread(next);
}

static inline struct thread *load_idle_thread(struct scheduler *sched) {
    /* Idle thread has no need to have a timeslice
     * No preemption will be occurring since nothing else runs */
    disable_timeslice();
    return sched->idle_thread;
}

static inline void change_timeslice(struct scheduler *sched,
                                    struct thread *next) {

    /* Only one thread is running - no timeslice needed */
    if (sched->thread_count == 0) {
        disable_timeslice();
        return;
    }

    if (THREAD_PRIO_IS_TIMESHARING(next->perceived_prio)) {
        /* Timesharing threads need timeslices */
        enable_timeslice();
    } else {
        /* RT threads do not share time*/
        disable_timeslice();
    }
}

void schedule(void) {
    struct scheduler *sched = get_this_core_sched();

    bool interrupts = spin_lock(&sched->lock);

    struct thread *curr = sched->current;
    struct thread *next = NULL;

    requeue_current_thread_if_runnable(sched, curr);

    /* Checks if we can steal, finds a victim, and tries to steal.
     * NULL is returned if any step was unsuccessful */
    struct thread *stolen = scheduler_try_do_steal(sched);

    next = stolen ? stolen : scheduler_pick_regular_thread(sched);

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

    load_thread(sched, next);

    spin_unlock(&sched->lock, interrupts);

    if (curr && curr->state != THREAD_STATE_IDLE_THREAD) {
        switch_context(&curr->regs, &next->regs);
    } else {
        /* Only `load_context` here since nothing was running,
         * typically only used in the very first yield or when
         * exiting the idle thread */
        load_context(&next->regs);
    }
}

void scheduler_yield() {
    bool were_enabled = are_interrupts_enabled();

    disable_interrupts();
    schedule();

    if (were_enabled)
        enable_interrupts();
    else
        disable_interrupts();
}
