#include <acpi/lapic.h>
#include <asm.h>
#include <console/printf.h>
#include <devices/registry.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <mp/mp.h>
#include <mutex.h>
#include <sch/sched.h>
#include <sleep.h>
#include <spin_lock.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <tests.h>

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
    while (1) {
        asm volatile("hlt");
    }
}

uint64_t scheduler_get_core_count() {
    return c_count;
}

/* TODO: no rdmsr */
struct thread *scheduler_get_curr_thread() {
    struct core *c = (void *) rdmsr(MSR_GS_BASE);
    return c->current_thread;
}

static inline void update_core_current_thread(struct thread *next) {
    struct core *c = (void *) rdmsr(MSR_GS_BASE);
    c->current_thread = next;
}

static inline void maybe_recompute_threshold(uint64_t core_id) {
    if (core_id == 0) {
        uint64_t val = atomic_load(&total_threads);
        work_steal_min_diff = compute_steal_threshold(val, c_count);
    }
}

static inline void stop_steal(struct scheduler *sched,
                              struct scheduler *victim) {
    if (victim)
        atomic_store(&victim->being_robbed, false);

    atomic_store(&sched->stealing_work, false);
    end_steal();
}

static void scheduler_save_thread(struct scheduler *sched, struct thread *curr,
                                  struct cpu_state *cpu) {
    if (curr) {
        memcpy(&curr->regs, cpu, sizeof(struct cpu_state));

        if (curr->state != RUNNING)
            return;

        curr->curr_core = -1;
        curr->time_in_level++;
        uint8_t level = curr->mlfq_level;
        uint64_t timeslice = 1ULL << level;

        if (curr->time_in_level >= timeslice) {
            curr->time_in_level = 0;
            if (level < MLFQ_LEVELS - 1)
                curr->mlfq_level++;
        }

        if (curr->state == RUNNING) {
            curr->state = READY;
            scheduler_add_thread(sched, curr, false, true, false);
        }
    }
}

static struct thread *scheduler_pick_regular_thread(struct scheduler *sched) {
    uint8_t bitmap = sched->queue_bitmap;

    while (bitmap) {
        int lvl = __builtin_ctz(bitmap);
        bitmap &= ~(1 << lvl);

        struct thread_queue *q = &sched->queues[lvl];
        if (!q->head)
            continue; // could be stale

        struct thread *start = q->head;
        struct thread *iter = start;

        do {
            if (iter->state == READY || iter->state == IDLE_THREAD)
                break;
            iter = iter->next;
        } while (iter != start);

        if (iter->state != READY)
            continue;

        struct thread *next = iter;

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

static void load_thread(struct scheduler *sched, struct thread *next,
                        struct cpu_state *cpu) {
    if (next) {
        sched->current = next;
        memcpy(cpu, &next->regs, sizeof(struct cpu_state));
        next->state = RUNNING;
        next->curr_core = get_sch_core_id();
    } else {
        sched->current = NULL;
    }
}

static inline void disable_timeslice() {
    uint64_t core_id = get_sch_core_id();
    k_printf("Core %u disabled the timeslice\n", core_id);
    lapic_timer_disable();
}

void scheduler_yield() {
    asm volatile("int $0x20");
}

void scheduler_enable_timeslice() {
    lapic_timer_enable();
}

static inline void begin_steal(struct scheduler *sched) {
    atomic_store(&sched->stealing_work, true);
}

static bool all_threads_unrunnable(struct scheduler *sched) {
    for (uint64_t lvl = 0; lvl < MLFQ_LEVELS; lvl++) {
        struct thread_queue *q = &sched->queues[lvl];
        if (!q->head)
            continue;

        struct thread *start = q->head;
        struct thread *iter = start;

        do {
            if (iter->state == READY)
                return false;
            iter = iter->next;
        } while (iter != start);
    }
    return true;
}

/* Resource locks in here do not enable interrupts */
void schedule(struct cpu_state *cpu) {
    uint64_t core_id = get_sch_core_id();
    struct scheduler *sched = local_schs[core_id];

    spin_lock_no_cli(&sched->lock);

    struct thread *curr = sched->current;
    struct thread *next = NULL;
    struct scheduler *victim = NULL;

    /* skip */
    if (!sched->active)
        goto end;

    /* core 0 will recompute the steal threshold */
    maybe_recompute_threshold(core_id);

    /* re-insert the running thread to its new level */
    scheduler_save_thread(sched, curr, cpu);

    /* check if we can steal */
    if (!scheduler_can_steal_work(sched))
        goto regular_schedule;

    if (!try_begin_steal())
        goto regular_schedule;

    /* attempt a steal */
    begin_steal(sched);
    victim = scheduler_pick_victim(sched);

    if (!victim) {
        /* victim cannot be stolen from - early abort */
        stop_steal(sched, victim);
        goto regular_schedule;
    }

    struct thread *stolen = scheduler_steal_work(victim);

    /* done stealing work now. another core can steal from us */
    stop_steal(sched, victim);

    /* work was successfully stolen */
    if (stolen) {
        next = stolen;
        goto load_new_thread;
    }

regular_schedule:
    next = scheduler_pick_regular_thread(sched);

load_new_thread:
    load_thread(sched, next, cpu);

    update_core_current_thread(next);

    if (!next) {
        disable_timeslice();
        goto end;
    }

    if (all_threads_unrunnable(sched) && next->state == IDLE_THREAD)
        disable_timeslice();

end:
    /* do not change interrupt status */
    spin_unlock_no_cli(&sched->lock);
}
