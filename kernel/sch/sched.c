#include <acpi/lapic.h>
#include <asm.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <mp/mp.h>
#include <sch/sched.h>
#include <spin_lock.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

struct scheduler **local_schs;
uint64_t c_count = 1;
/* This guy helps us figure out if the scheduler's load is
   enough of a portion of the global load to not steal work*/
atomic_int global_load = 0;

/* This is how many cores can be stealing work at once,
 * it is half the core count */
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
    uint64_t core_id = get_sch_core_id();
    while (1) {
     //   k_printf("Core %llu is in the idle task empty loop\n", core_id);
        asm volatile("hlt");
    }
}

void k_sch_other() {
    uint64_t core_id = get_sch_core_id();
    while (1) {
      //  k_printf("Core %llu is in the other idle task empty loop\n", core_id);
        asm volatile("hlt");
    }
}

void scheduler_update_loads(struct scheduler *sched) {
    int64_t old_load = sched->load;
    int64_t new_load = scheduler_compute_load(sched, ALPHA_SCALE, BETA_SCALE);
    int64_t delta = new_load - old_load;
    sched->load = new_load;
    atomic_fetch_add(&global_load, delta);
}

/* Resource locks in here do not enable interrupts */
void schedule(struct cpu_state *cpu) {
    uint64_t core_id = get_sch_core_id();
    struct scheduler *sched = local_schs[core_id];
    spin_lock(&sched->lock);

    /* make sure these are actually our core IDs */
    if (sched->core_id == -1)
        sched->core_id = core_id;

    if (!sched->active) {
        LAPIC_REG(LAPIC_REG_EOI) = 0;
        return;
    }

    /* core 0 will recompute the steal threshold */

    if (core_id == 0) {
        uint64_t val = atomic_load(&total_threads);
        work_steal_min_diff = compute_steal_threshold(val, c_count);
    }

    struct thread *curr = sched->current;

    /* re-insert the running thread to its new level */
    if (curr && curr->state == RUNNING) {
        memcpy(&curr->regs, cpu, sizeof(struct cpu_state));
        curr->state = READY;
        curr->time_in_level++;

        uint8_t level = curr->mlfq_level;
        uint64_t timeslice = 1ULL << level; // TODO: Statically calculate these

        if (curr->time_in_level >= timeslice) {
            curr->time_in_level = 0;

            // Demote if not at lowest level
            if (level < MLFQ_LEVELS - 1) {
                curr->mlfq_level++;
            }
        }

        /* Re-insert the thread into its new level
         * `false, true` here says, do NOT change interrupt status,
         * and the resource is already locked */
        scheduler_add_thread(sched, curr, false, true, false);
    }

    struct thread *next = NULL;
    bool work_stolen = false;

    // attempt a work steal
    if (scheduler_can_steal_work(sched)) {
        if (try_begin_steal()) {

            atomic_store(&sched->stealing_work, true);
            struct scheduler *victim = scheduler_pick_victim(sched);

            if (!victim) {
                /* this means that every core is busy stealing work
                 * or is busy getting stolen from, and thus we cannot
                 * steal work from any victim. continuing with regular
                 * scheduling */
                atomic_store(&sched->stealing_work, false);
                end_steal();
                goto regular_schedule;
            }

            k_printf(ANSI_GREEN "Core %u is stealing from core %u\n" ANSI_RESET,
                     core_id, victim->core_id);
            uint64_t val = atomic_load(&global_load);
            k_printf(ANSI_BLUE "Core %u has a load of %u, victim has a load of "
                               "%u, global load is %u\n" ANSI_RESET,
                     core_id, sched->load, victim->load, val);

            struct thread *stolen = scheduler_steal_work(victim);
            atomic_store(&victim->being_robbed, false);
            atomic_store(&sched->stealing_work, false);
            end_steal();

            if (stolen) {
                next = stolen;
                work_stolen = true;

                goto load_new_thread;

            } else {
                goto regular_schedule;
            }
        } else {

            goto regular_schedule;
        }
    }

    /* work was successfully stolen */
    if (next)
        goto load_new_thread;

regular_schedule:
    // Pick next READY thread from highest non-empty level
    for (int lvl = 0; lvl < MLFQ_LEVELS; lvl++) {
        struct thread_queue *q = &sched->queues[lvl];

        if (!q->head)
            continue;

        struct thread *start = q->head;
        struct thread *iter = start;

        do {
            if (iter->state == READY) {
                next = iter;
                break;
            }
            iter = iter->next;
        } while (iter != start);

        if (next) {
            // Remove from queue
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

            next->next = NULL;
            next->prev = NULL;
            break;
        }
    }

load_new_thread:
    if (next) {
        sched->current = next;
        memcpy(cpu, &next->regs, sizeof(struct cpu_state));
        next->state = RUNNING;
    } else {
        sched->current = NULL;
        k_panic("No threads to run! State should not be reached!\n");
    }

    struct core *c = (void *) rdmsr(MSR_GS_BASE);
    c->current_thread = next;
    LAPIC_REG(LAPIC_REG_EOI) = 0;

    /* done stealing work now. another core can steal from us */
    if (work_stolen)
        atomic_store(&sched->stealing_work, false);

    /* do not change interrupt status */
    spin_unlock(&sched->lock, false);
}
