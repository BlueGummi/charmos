#include <acpi/lapic.h>
#include <asm.h>
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
uint64_t c_count = 1;
/* This guy helps us figure out if the scheduler's load is
   enough of a portion of the global load to not steal work*/
atomic_int global_load = 0;

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
    scheduler_get_curr_thread()->flags = NO_STEAL;
    k_info("MAIN", K_INFO, "Device setup");
    registry_setup();
    tests_run();
    k_info("MAIN", K_INFO, "Boot OK");
    while (1) {
        asm volatile("hlt");
    }
}

void k_sch_idle() {
    scheduler_get_curr_thread()->flags = NO_STEAL;
    scheduler_get_curr_thread()->state = SLEEPING;
    while (1) {
        enable_interrupts();
        asm volatile("hlt");
    }
}

uint64_t scheduler_get_core_count() {
    return c_count;
}

void scheduler_wake(struct thread *t) {
    t->state = READY;

    /* boost */
    t->mlfq_level = 0;
    t->time_in_level = 0;
    uint64_t c = t->curr_core;
    scheduler_put_back(t);
    lapic_send_ipi(c, SCHEDULER_ID);
}

static __always_inline void update_core_current_thread(struct thread *next) {
    struct core *c = global_cores[get_this_core_id()];
    c->current_thread = next;
}

static __always_inline void maybe_recompute_threshold(uint64_t core_id) {
    if (core_id == 0) {
        uint64_t val = atomic_load(&total_threads);
        work_steal_min_diff = compute_steal_threshold(val, c_count);
    }
}

static __always_inline bool try_begin_steal() {
    unsigned current = atomic_load(&active_stealers);
    while (current < max_concurrent_stealers) {
        if (atomic_compare_exchange_weak(&active_stealers, &current,
                                         current + 1)) {
            return true;
        }
    }
    return false;
}

static __always_inline void end_steal() {
    atomic_fetch_sub(&active_stealers, 1);
}

static __always_inline void stop_steal(struct scheduler *sched,
                                       struct scheduler *victim) {
    if (victim)
        atomic_store(&victim->being_robbed, false);

    atomic_store(&sched->stealing_work, false);
    end_steal();
}

static __always_inline void scheduler_save_thread(struct scheduler *sched,
                                                  struct thread *curr) {
    maybe_recompute_threshold(get_this_core_id());
    if (curr && curr->state == RUNNING) {
        curr->curr_core = -1;
        curr->time_in_level++;
        uint8_t level = curr->mlfq_level;
        uint64_t timeslice = 1ULL << level;

        if (curr->time_in_level >= timeslice) {
            curr->time_in_level = 0;
            if (level < MLFQ_LEVELS - 1)
                curr->mlfq_level++;
        }

        curr->state = READY;
        bool change_interrupts = false;
        bool locked = true;
        bool new = false;
        scheduler_add_thread(sched, curr, change_interrupts, locked, new);
    }
}

static __always_inline struct thread *
scheduler_pick_regular_thread(struct scheduler *sched) {
    uint8_t bitmap = sched->queue_bitmap;

    while (bitmap) {
        int lvl = __builtin_ctz(bitmap);
        bitmap &= ~(1 << lvl);

        struct thread_queue *q = &sched->queues[lvl];

        struct thread *next = q->head;

        if (next == q->head && next == q->tail) {
            q->head = NULL;
            q->tail = NULL;
        } else if (next == q->head) {
            q->head = next->next;
            q->head->prev = q->tail;
            q->tail->next = q->head;
        } else if (next == q->tail) {
            q->tail = next->prev;
            q->tail->next = q->head;
            q->head->prev = q->tail;
        } else {
            next->prev->next = next->next;
            next->next->prev = next->prev;
        }

        if (q->head == NULL)
            sched->queue_bitmap &= ~(1 << lvl);

        next->next = NULL;
        next->prev = NULL;
        return next;
    }

    return NULL;
}

static __always_inline void load_thread(struct scheduler *sched,
                                        struct thread *next) {
    if (next) {
        sched->current = next;
        next->state = RUNNING;
        next->curr_core = get_this_core_id();
    } else {
        sched->current = NULL;
    }
}

static __always_inline void disable_timeslice() {
    if (lapic_timer_is_enabled()) {
        lapic_timer_disable();
    }
}

static __always_inline void enable_timeslice() {
    scheduler_enable_timeslice();
}

static __always_inline struct thread *
load_idle_thread(struct scheduler *sched) {
    disable_timeslice();
    return sched->idle_thread;
}

void scheduler_yield() {
    bool were_enabled = are_interrupts_enabled();
    disable_interrupts();

    schedule();

    if (were_enabled)
        enable_interrupts();
}

void scheduler_enable_timeslice() {
    if (!lapic_timer_is_enabled()) {
        lapic_timer_enable();
    }
}

static __always_inline void begin_steal(struct scheduler *sched) {
    atomic_store(&sched->stealing_work, true);
}

static __always_inline struct thread *try_do_steal(struct scheduler *sched) {
    if (!scheduler_can_steal_work(sched))
        return NULL;

    if (!try_begin_steal())
        return NULL;

    begin_steal(sched);
    struct scheduler *victim = scheduler_pick_victim(sched);

    if (!victim) {
        stop_steal(sched, victim);
        return NULL;
    }

    struct thread *stolen = scheduler_steal_work(victim);
    stop_steal(sched, victim);

    return stolen;
}

void schedule(void) {
    struct scheduler *sched = get_this_core_sched();

    bool interrupts = spin_lock(&sched->lock);

    struct thread *curr = sched->current;
    struct thread *next = NULL;
    scheduler_save_thread(sched, curr);

    struct thread *stolen = try_do_steal(sched);

    next = stolen ? stolen : scheduler_pick_regular_thread(sched);

    if (!next) {
        next = load_idle_thread(sched);
    } else {
        if (curr == next)
            disable_timeslice();
        else
            enable_timeslice();
    }

    load_thread(sched, next);
    update_core_current_thread(next);
    spin_unlock(&sched->lock, interrupts);

    if (curr && next)
        switch_context(&curr->regs, &next->regs);
    else if (next)
        load_context(&next->regs);
}
