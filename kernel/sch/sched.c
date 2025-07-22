#include <acpi/lapic.h>
#include <asm.h>
#include <boot/stage.h>
#include <compiler.h>
#include <console/printf.h>
#include <int/idt.h>
#include <mp/mp.h>
#include <registry.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <tests.h>

#include "mp/core.h"
#include "sch/thread.h"

struct scheduler **local_schs;

/* This is how many cores can be stealing work at once */
uint32_t max_concurrent_stealers = 0;

/* This is how many cores are attempting a work steal right now.
 * If this is above the maximum concurrent stealers, we will not
 * attempt any work steals. */
atomic_uint active_stealers = 0;

/* total threads running across all cores right now */
atomic_uint total_threads = 0;

/* How much more work the victim must be doing than the stealer
 * for the stealer to go through with the steal. */
int64_t work_steal_min_diff = 130;

void k_sch_main() {
    k_info("MAIN", K_INFO, "Device setup");
    registry_setup();
    global.current_bootstage = BOOTSTAGE_LATE_DEVICES;
    tests_run();
    k_info("MAIN", K_INFO, "Boot OK");
    global.current_bootstage = BOOTSTAGE_COMPLETE;

    while (1) {
        asm volatile("hlt");
    }
}

void k_sch_idle() {
    enable_interrupts();
    while (1) {
        enable_interrupts();
        asm volatile("hlt");
    }
}

void scheduler_wake(struct thread *t) {
    atomic_store(&t->state, THREAD_STATE_READY);
    /* boost */

    t->prio = THREAD_PRIO_MAX_BOOST(t->prio);
    t->time_in_level = 0;
    uint64_t c = t->curr_core;
    scheduler_put_back(t);

    lapic_send_ipi(c, SCHEDULER_ID);
}

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

static inline void maybe_recompute_threshold(uint64_t core_id) {
    if (core_id == 0) {
        uint64_t val = atomic_load(&total_threads);
        work_steal_min_diff = scheduler_compute_steal_threshold(val);
    }
}

static inline enum thread_priority ts_new_prio(enum thread_priority curr) {
    return curr == THREAD_PRIO_LOW ? THREAD_PRIO_HIGH : curr + 1;
}

static inline void do_thread_prio_decay(struct thread *thread) {
    thread->time_in_level = 0;
    if (THREAD_PRIO_IS_TIMESHARING(thread->prio))
        thread->prio = ts_new_prio(thread->prio);

    if (thread->prio == THREAD_PRIO_URGENT)
        thread->prio = THREAD_PRIO_HIGH;
}

static inline void update_thread_before_save(struct thread *thread) {
    atomic_store(&thread->state, THREAD_STATE_READY);
    thread->curr_core = -1;
    thread->time_in_level++;

    if (thread->time_in_level >= TICKS_FOR_PRIO(thread->prio))
        do_thread_prio_decay(thread);
}

static inline void do_re_enqueue_thread(struct scheduler *sched,
                                        struct thread *thread) {
    bool change_interrupts = false;
    bool locked = true;
    bool new = false;
    scheduler_add_thread(sched, thread, change_interrupts, locked, new);
}

static inline void do_save_thread(struct scheduler *sched,
                                  struct thread *curr) {
    update_thread_before_save(curr);
    do_re_enqueue_thread(sched, curr);
}

static inline void requeue_current_thread_if_runnable(struct scheduler *sched,
                                                      struct thread *curr) {
    maybe_recompute_threshold(get_this_core_id());
    if (curr && atomic_load(&curr->state) == THREAD_STATE_RUNNING)
        do_save_thread(sched, curr);
}

static struct thread *scheduler_pick_regular_thread(struct scheduler *sched) {
    uint8_t bitmap = atomic_load(&sched->queue_bitmap);

    while (bitmap) {
        int lvl = __builtin_ctz(bitmap);
        bitmap &= ~(1 << lvl);

        struct thread_queue *q = &sched->queues[lvl];
        struct thread *next = q->head;

        if (next == q->tail) {
            q->head = NULL;
            q->tail = NULL;
        } else {
            q->head = next->next;
            q->head->prev = q->tail;
            q->tail->next = q->head;
        }

        if (q->head == NULL)
            sched->queue_bitmap &= ~(1 << lvl);

        next->next = NULL;
        next->prev = NULL;
        return next;
    }

    return NULL;
}

static void load_thread(struct scheduler *sched, struct thread *next) {
    if (next) {
        sched->current = next;
        atomic_store(&next->state, THREAD_STATE_RUNNING);
        next->curr_core = get_this_core_id();
    } else {
        sched->current = NULL;
    }
}

static inline struct thread *load_idle_thread(struct scheduler *sched) {
    disable_timeslice();
    return sched->idle_thread;
}

static inline void change_timeslice(struct scheduler *sched,
                                    struct thread *next) {
    if (sched->thread_count == 1) {
        disable_timeslice();
        return;
    }

    if (THREAD_PRIO_IS_TIMESHARING(next->prio))
        enable_timeslice();
    else
        disable_timeslice();
}

void schedule(void) {
    struct scheduler *sched = get_this_core_sched();

    bool interrupts = spin_lock(&sched->lock);

    struct thread *curr = sched->current;
    struct thread *next = NULL;
    requeue_current_thread_if_runnable(sched, curr);

    struct thread *stolen = scheduler_try_do_steal(sched);

    next = stolen ? stolen : scheduler_pick_regular_thread(sched);

    if (!next) {
        next = load_idle_thread(sched);
    } else {
        change_timeslice(sched, next);
    }

    load_thread(sched, next);
    update_core_current_thread(next);

    spin_unlock(&sched->lock, interrupts);

    if (curr && next)
        switch_context(&curr->regs, &next->regs);
    else
        load_context(&next->regs);
}

void scheduler_yield() {
    bool were_enabled = are_interrupts_enabled();
    disable_interrupts();

    schedule();

    if (were_enabled)
        enable_interrupts();
}
