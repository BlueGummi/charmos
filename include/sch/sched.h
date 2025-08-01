#pragma once
#include <charmos.h>
#include <mp/core.h>
#include <sch/thread.h>
#include <stdbool.h>
#include <sync/spin_lock.h>

#define MLFQ_LEVELS 6

#define WORK_STEAL_THRESHOLD                                                   \
    75ULL /* How little work the core needs to be                              \
           * doing to try to steal work from another                           \
           * core. This means "% of the average"                               \
           */

#define SCHEDULER_DEFAULT_WORK_STEAL_MIN_DIFF 130

enum idle_thread_state {
    IDLE_THREAD_WORK_STEAL = 0, /* Attempt to do a thread steal */
    IDLE_THREAD_SLEEP = 1,      /* Enter sleep state */
};

struct idle_thread_data {
    _Atomic enum idle_thread_state state;

    atomic_bool woken_from_timer;
    atomic_bool did_work_recently;
    atomic_uint_fast64_t last_entry_ms;
    uint64_t last_exit_ms;
};

struct scheduler {
    atomic_bool timeslice_enabled;

    struct thread_queue urgent_threads;

    struct rbt thread_rbt;
    struct thread_queue ts_threads;

    struct thread_queue rt_threads;
    struct thread_queue bg_threads;

    struct thread *current;
    uint64_t thread_count; // Also used for load estimate
    int64_t core_id;
    atomic_bool being_robbed;
    atomic_bool stealing_work;
    struct spinlock lock;
    struct thread *idle_thread;
    struct idle_thread_data idle_thread_data;
    atomic_uint_fast8_t queue_bitmap;
};

void scheduler_init();

void scheduler_add_thread(struct scheduler *sched, struct thread *thread,
                          bool already_locked);

void scheduler_rm_thread(struct scheduler *sched, struct thread *thread,
                         bool already_locked);
void schedule(void);
void k_sch_main(void);
void scheduler_idle_main(void);
void scheduler_enable_timeslice();
void scheduler_disable_timeslice();
void scheduler_yield();
void scheduler_enqueue(struct thread *t);
void scheduler_enqueue_on_core(struct thread *t, uint64_t core_id);
void scheduler_wake(struct thread *t, enum thread_priority prio,
                    enum thread_wake_reason reason);
void scheduler_take_out(struct thread *t);
void switch_context(struct cpu_context *old, struct cpu_context *new);
void load_context(struct cpu_context *new);

bool scheduler_can_steal_work(struct scheduler *sched);
uint64_t scheduler_compute_steal_threshold();
struct thread *scheduler_try_do_steal(struct scheduler *sched);

struct scheduler *scheduler_pick_victim(struct scheduler *self);
struct thread *scheduler_steal_work(struct scheduler *victim);

struct scheduler_data {
    uint32_t max_concurrent_stealers;
    atomic_uint active_stealers;
    atomic_uint total_threads;
    int64_t steal_min_diff;
};

extern struct scheduler_data scheduler_data;

/* TODO: no rdmsr */
static inline struct thread *scheduler_get_curr_thread() {
    return global.cores[get_this_core_id()]->current_thread;
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

static inline struct idle_thread_data *get_this_core_idle_thread(void) {
    return &get_this_core_sched()->idle_thread_data;
}

static inline void scheduler_decrement_thread_count(struct scheduler *sched) {
    sched->thread_count--;
    atomic_fetch_sub(&scheduler_data.total_threads, 1);
}

static inline void scheduler_increment_thread_count(struct scheduler *sched) {
    sched->thread_count++;
    atomic_fetch_add(&scheduler_data.total_threads, 1);
}

static inline struct thread_queue *
scheduler_get_this_thread_queue(struct scheduler *sched,
                                enum thread_priority prio) {
    switch (prio) {
    case THREAD_PRIO_URGENT: return &sched->urgent_threads;
    case THREAD_PRIO_RT: return &sched->rt_threads;
    case THREAD_PRIO_HIGH:
    case THREAD_PRIO_MID:
    case THREAD_PRIO_LOW: return &sched->ts_threads;
    case THREAD_PRIO_BACKGROUND: return &sched->bg_threads;
    }
    k_panic("unreachable!\n");
}

static inline void scheduler_wake_from_io_block(struct thread *t) {
    scheduler_wake(t, THREAD_PRIO_URGENT, THREAD_WAKE_REASON_BLOCKING_IO);
}

#define TICKS_FOR_PRIO(level) (level == THREAD_PRIO_LOW ? 64 : 1ULL << level)
