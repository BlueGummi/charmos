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
static uint64_t c_count = 1;
static bool scheduler_can_steal_work(struct scheduler *sched);
static uint64_t scheduler_compute_load(struct scheduler *sched,
                                       uint64_t alpha_scaled,
                                       uint64_t beta_scaled);

static struct scheduler *scheduler_pick_victim(struct scheduler *self);
static struct thread *scheduler_steal_work(struct scheduler *victim);

static bool try_begin_steal();

static void end_steal();

/* This guy helps us figure out if the scheduler's load is
   enough of a portion of the global load to not steal work*/
static _Atomic uint64_t global_load = 0;

/* This is how many cores can be stealing work at once,
 * it is half the core count */
static uint32_t max_concurrent_stealers = 0;

/* This is how many cores are attempting a work steal right now.
 * If this is above the maximum concurrent stealers, we will not
 * attempt any work steals. */
static atomic_uint active_stealers = 0;

void k_sch_main() {
    uint64_t core_id = get_sch_core_id();
    k_printf("Core %llu is in the idle task\n", core_id);
    while (1) {
        k_printf("Core %llu is in the idle task empty loop\n", core_id);
        asm volatile("hlt");
    }
}

void k_sch_other() {
    uint64_t core_id = get_sch_core_id();
    k_printf("Core %llu is in the other idle task\n", core_id);
    while (1) {
        k_printf("Core %llu is in the other idle task empty loop\n", core_id);
        asm volatile("hlt");
    }
}

/* Resource locks in here do not enable interrupts */
void schedule(struct cpu_state *cpu) {
    uint64_t core_id = get_sch_core_id();
    struct scheduler *sched = local_schs[core_id];

    if (!sched->active) {
        LAPIC_REG(LAPIC_REG_EOI) = 0;
        return;
    }

    spin_lock(&sched->lock);
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
        scheduler_add_thread(sched, curr, false, true);
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

            k_printf("Core %u has picked a suitable victim\n", core_id);
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

static bool try_begin_steal() {
    unsigned current = atomic_load(&active_stealers);
    while (current < max_concurrent_stealers) {
        if (atomic_compare_exchange_weak(&active_stealers, &current,
                                         current + 1)) {
            return true;
        }
    }
    return false;
}

static void end_steal() {
    atomic_fetch_sub(&active_stealers, 1);
}

static struct scheduler *scheduler_pick_victim(struct scheduler *self) {
    // self->stealing_work should already be set before this is called
    /* Ideally, we want to steal from our busiest core */
    uint64_t max_load = 0;
    struct scheduler *victim = NULL;

    for (uint64_t i = 0; i < c_count - 1; i++) {
        struct scheduler *potential_victim = local_schs[i];

        /* duh.... */
        if (potential_victim == self)
            continue;

        bool victim_busy = atomic_load(&potential_victim->being_robbed) ||
                           atomic_load(&potential_victim->stealing_work);

        if (victim_busy)
            continue;

        if (potential_victim->load > max_load) {
            max_load = potential_victim->load;
            victim = potential_victim;
        }
    }

    if (victim)
        atomic_store(&victim->being_robbed, true);

    return victim;
}

static bool scheduler_can_steal_work(struct scheduler *sched) {
    if (c_count == 0) {
        k_panic("Why are there no cores on your machine?\n");
    }

    uint64_t val = atomic_load(&global_load);
    uint64_t avg_core_load = val / c_count;

    // steal if this core's load is less than WORK_STEAL_THRESHOLD% of average
    uint64_t threshold_load =
        ((avg_core_load * WORK_STEAL_THRESHOLD) / 100ULL) ?: 1;

    return (sched->load < threshold_load);
}

static uint64_t scheduler_compute_load(struct scheduler *sched,
                                       uint64_t alpha_scaled,
                                       uint64_t beta_scaled) {
    if (!sched)
        return 0;

    uint64_t ready_count = 0;
    uint64_t weighted_sum = 0;

    for (int level = 0; level < MLFQ_LEVELS; level++) {
        struct thread_queue *q = &sched->queues[level];
        if (!q->head)
            continue;

        struct thread *start = q->head;
        struct thread *current = start;

        do {
            if (current->state == READY) {
                ready_count++;
                // Weight lower levels more heavily
                weighted_sum += (MLFQ_LEVELS - level);
            }
            current = current->next;
        } while (current != start);
    }

    if (ready_count == 0)
        return 0;

    // floating point math is bad so we scale it
    uint64_t load_scaled =
        ready_count *
        (alpha_scaled + (beta_scaled * weighted_sum) / ready_count);

    return load_scaled;
}

void scheduler_init(uint64_t core_count) {
    c_count = core_count;
    max_concurrent_stealers = c_count / 2;

    /* I mean, if we have one core and that core wants
     * to steal work from itself, go ahead? */
    if (max_concurrent_stealers == 0)
        max_concurrent_stealers = 1;

    local_schs = kmalloc(sizeof(struct scheduler *) * core_count);
    if (!local_schs)
        k_panic("Could not allocate scheduler pointer array\n");

    for (uint64_t i = 0; i < core_count; i++) {
        struct scheduler *s = kmalloc_aligned(sizeof(struct scheduler), 4096);
        if (!s)
            k_panic("Could not allocate scheduler %lu\n", i);

        if ((uintptr_t) s % 4096 != 0) {
            k_panic("Scheduler %lu is not page-aligned: 0x%lx\n", i,
                    (uintptr_t) s);
        }

        s->active = true;
        s->thread_count = 0;
        s->tick_counter = 0;

        for (int lvl = 0; lvl < MLFQ_LEVELS; lvl++) {
            s->queues[lvl].head = NULL;
            s->queues[lvl].tail = NULL;
        }

        struct thread *t = thread_create(k_sch_main);
        struct thread *t0 = thread_create(k_sch_other);
        scheduler_add_thread(s, t, false, false);
        scheduler_add_thread(s, t0, false, false);

        if (i == 0) {
            for (int j = 0; j < 5; j++) {
                struct thread *t1 = thread_create(k_sch_main);
                scheduler_add_thread(s, t1, false, false);
            }
        }

        s->load = scheduler_compute_load(s, 700, 300);
        atomic_fetch_add(&global_load, s->load);
        local_schs[i] = s;
    }
}

void scheduler_add_thread(struct scheduler *sched, struct thread *task,
                          bool change_interrupts, bool already_locked) {
    if (!sched || !task)
        return;

    bool ints;
    if (!already_locked)
        ints = spin_lock(&sched->lock);

    uint8_t level = task->mlfq_level;
    struct thread_queue *q = &sched->queues[level];

    task->next = NULL;
    task->prev = NULL;

    if (!q->head) {
        q->head = task;
        q->tail = task;
        task->next = task;
        task->prev = task;
    } else {
        task->prev = q->tail;
        task->next = q->head;
        q->tail->next = task;
        q->head->prev = task;
        q->tail = task;
    }

    atomic_fetch_sub(&global_load, sched->load);
    sched->load = scheduler_compute_load(sched, 700, 300);
    atomic_fetch_add(&global_load, sched->load);
    sched->thread_count++;

    if (!already_locked)
        spin_unlock(&sched->lock, change_interrupts ? ints : false);
}

/* We do not enable interrupts here because this is only ever
 * called from the `schedule()` function which should not enable
 * interrupts inside of itself */

static struct thread *scheduler_steal_work(struct scheduler *victim) {
    if (!victim || victim->thread_count == 0)
        return NULL;

    spin_lock(&victim->lock);
    for (int level = MLFQ_LEVELS - 1; level >= 0; level--) {
        struct thread_queue *q = &victim->queues[level];

        if (!q->head)
            continue;

        struct thread *start = q->head;
        struct thread *current = start;

        do {
            if (current->state == READY) {
                if (current == q->head && current == q->tail) {
                    q->head = NULL;
                    q->tail = NULL;
                } else if (current == q->head) {
                    q->head = current->next;
                    q->head->prev = q->tail;
                    q->tail->next = q->head;
                } else if (current == q->tail) {
                    q->tail = current->prev;
                    q->tail->next = q->head;
                    q->head->prev = q->tail;
                } else {
                    current->prev->next = current->next;
                    current->next->prev = current->prev;
                }

                current->next = NULL;
                current->prev = NULL;
                victim->thread_count--;

                /* do not re-enable interrupts!!! */
                spin_unlock(&victim->lock, false);
                return current;
            }

            current = current->next;
        } while (current != start);
    }

    spin_unlock(&victim->lock, false);
    return NULL; // Nothing to steal
}

void scheduler_rm_thread(struct scheduler *sched, struct thread *task,
                         bool change_interrupts, bool already_locked) {
    if (!sched || !task)
        return;

    bool ints;
    if (!already_locked)
        ints = spin_lock(&sched->lock);

    uint8_t level = task->mlfq_level;
    struct thread_queue *q = &sched->queues[level];

    if (!q->head) {
        if (!already_locked)
            spin_unlock(&sched->lock, change_interrupts ? ints : false);

        return;
    }

    if (q->head == q->tail && q->head == task) {
        q->head = NULL;
        q->tail = NULL;
    } else if (q->head == task) {
        q->head = q->head->next;
        q->head->prev = q->tail;
        q->tail->next = q->head;
    } else if (q->tail == task) {
        q->tail = q->tail->prev;
        q->tail->next = q->head;
        q->head->prev = q->tail;
    } else {
        struct thread *current = q->head->next;
        while (current != q->head && current != task)
            current = current->next;

        if (current == task) {
            current->prev->next = current->next;
            current->next->prev = current->prev;
        }
    }

    thread_free(task);
    atomic_fetch_sub(&global_load, sched->load);
    sched->load = scheduler_compute_load(sched, 700, 300);
    atomic_fetch_add(&global_load, sched->load);
    sched->thread_count--;

    if (!already_locked)
        spin_unlock(&sched->lock, change_interrupts ? ints : false);
}
