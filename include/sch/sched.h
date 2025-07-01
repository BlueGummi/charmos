#pragma once
#include <sch/thread.h>
#include <spin_lock.h>
#include <stdbool.h>

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
    uint8_t queue_bitmap;
};

void scheduler_init(uint64_t core_count);
void scheduler_add_thread(struct scheduler *sched, struct thread *thread,
                          bool change_interrupts, bool already_locked,
                          bool is_new_thread);
void scheduler_rm_thread(struct scheduler *sched, struct thread *thread,
                         bool change_interrupts, bool already_locked);
void schedule(struct cpu_state *cpu);
void k_sch_main();
void k_sch_idle();
void scheduler_enable_timeslice();
void scheduler_yield();
void scheduler_enqueue(struct thread *t);
void scheduler_put_back(struct thread *t);
void scheduler_wake_up(struct thread_queue *q);

bool scheduler_can_steal_work(struct scheduler *sched);
uint64_t compute_steal_threshold(uint64_t threads, uint64_t core_count);

struct scheduler *scheduler_pick_victim(struct scheduler *self);
struct thread *scheduler_steal_work(struct scheduler *victim);
struct thread *scheduler_get_curr_thread();

bool try_begin_steal();
void end_steal();

extern struct scheduler global_sched;
extern struct scheduler **local_schs;
extern uint32_t max_concurrent_stealers;
extern atomic_uint active_stealers;
extern atomic_uint total_threads;
extern int64_t work_steal_min_diff;
extern uint64_t c_count;

#define CLI asm volatile("cli")
#define STI asm volatile("sti")
