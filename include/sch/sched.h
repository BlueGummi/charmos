#pragma once
#include <charmos.h>
#include <mp/core.h>
#include <sch/thread.h>
#include <stdbool.h>
#include <sync/spin_lock.h>

#define MLFQ_LEVELS 5 // Number of priority queues (0 = highest priority)

#define WORK_STEAL_THRESHOLD                                                   \
    75ULL /* How little work the core needs to be                              \
           * doing to try to steal work from another                           \
           * core. This means "% of the average"                               \
           */

struct scheduler {
    bool active;
    struct thread_queue queues[MLFQ_LEVELS]; // MLFQ queues
    struct thread *current;
    uint64_t thread_count; // Also used for load estimate
    int64_t core_id;
    uint64_t tick_counter; // Global tick count for periodic rebalance
    atomic_bool being_robbed;
    atomic_bool stealing_work;
    struct spinlock lock;
    struct thread *idle_thread;
    atomic_uint_fast8_t queue_bitmap;
};

void scheduler_init();

/* TODO: Use these internally, no need to worry about the bool parameters */
void scheduler_add_thread(struct scheduler *sched, struct thread *thread,
                          bool change_interrupts, bool already_locked,
                          bool is_new_thread);
void scheduler_rm_thread(struct scheduler *sched, struct thread *thread,
                         bool change_interrupts, bool already_locked);
void schedule(void);
void k_sch_main(void);
void k_sch_idle(void);
void scheduler_enable_timeslice();
void scheduler_disable_timeslice();
void scheduler_yield();
void scheduler_enqueue(struct thread *t);
void scheduler_enqueue_on_core(struct thread *t, uint64_t core_id);
void scheduler_put_back(struct thread *t);
void scheduler_wake(struct thread *t);
void scheduler_take_out(struct thread *t);
void switch_context(struct context *old, struct context *new);
void load_context(struct context *new);

bool scheduler_can_steal_work(struct scheduler *sched);
uint64_t scheduler_compute_steal_threshold(uint64_t threads);
struct thread *scheduler_try_do_steal(struct scheduler *sched);

struct scheduler *scheduler_pick_victim(struct scheduler *self);
struct thread *scheduler_steal_work(struct scheduler *victim);

extern uint32_t max_concurrent_stealers;
extern atomic_uint active_stealers;
extern atomic_uint total_threads;
extern int64_t work_steal_min_diff;

/* TODO: no rdmsr */
static inline struct thread *scheduler_get_curr_thread() {
    struct core *c = (void *) rdmsr(MSR_GS_BASE);
    return c->current_thread;
}

static inline struct thread *thread_spawn(void (*entry)(void)) {
    struct thread *t = thread_create(entry);
    scheduler_enqueue(t);
    return t;
}

static inline struct thread *thread_spawn_on_core(void (*entry)(void),
                                                  uint64_t core_id) {
    struct thread *t = thread_create(entry);
    scheduler_enqueue_on_core(t, core_id);
    return t;
}

static inline struct scheduler *get_this_core_sched(void) {
    return global.schedulers[get_this_core_id()];
}

#define TICKS_FOR_PRIO(level) (1ULL << level)
