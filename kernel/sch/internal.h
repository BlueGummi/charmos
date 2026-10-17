#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <enforce.h>
#include <math/bit.h>
#include <math/min_max.h>
#include <sch/sched.h>
#include <thread/apc.h>
#include <thread/thread.h>
#include <time/time.h>

static inline void scheduler_set_queue_bitmap(struct scheduler *sched,
                                              uint8_t prio) {
    uint8_t mask = 0;
    mask = BIT_SET(mask, prio);
    atomic_fetch_or(&sched->queue_bitmap, mask);
}

static inline void scheduler_clear_queue_bitmap(struct scheduler *sched,
                                                uint8_t prio) {
    uint8_t mask = UINT8_MAX;
    mask = BIT_CLEAR(mask, prio);
    atomic_fetch_and(&sched->queue_bitmap, mask);
}

static inline uint8_t scheduler_get_bitmap(struct scheduler *sched) {
    return atomic_load(&sched->queue_bitmap);
}

static inline struct scheduler *smp_core_scheduler(void) {
    return global.schedulers[smp_id(TOPC_IRQL)];
}

static inline struct idle_thread_data *smp_core_idle_thread(void) {
    return &smp_core_scheduler()->idle_thread_data;
}

static inline bool thread_exhausted_period(struct scheduler *sched,
                                           struct thread *thread) {
    return thread->completed_period == sched->current_period;
}

#ifdef DEBUG_SCHED
void scheduler_debug_validate(struct scheduler *sched);

static inline void scheduler_debug_note_progress(struct scheduler *sched) {
    atomic_store_rlx(&sched->debug_progress_ms, (uint64_t) time_get_ms());
}
#else
static inline void scheduler_debug_validate(struct scheduler *sched) {
    cc_ignore(sched);
}

static inline void scheduler_debug_note_progress(struct scheduler *sched) {
    cc_ignore(sched);
}
#endif

static inline void scheduler_decrement_thread_count(struct scheduler *sched,
                                                    struct thread *t) {
    sched->total_thread_count--;
    sched->thread_count[t->perceived_prio_class]--;

    if (sched->thread_count[t->perceived_prio_class] == 0)
        scheduler_clear_queue_bitmap(sched, t->perceived_prio_class);

    if (t->effective_priority == THREAD_PRIO_CLASS_TIMESHARE)
        sched->total_weight -= t->weight;

    atomic_dec(&scheduler_data.total_threads);
}

static inline void scheduler_increment_thread_count(struct scheduler *sched,
                                                    struct thread *t) {
    if (sched->total_thread_count++ == 0)
        scheduler_debug_note_progress(sched);

    sched->thread_count[t->perceived_prio_class]++;
    scheduler_set_queue_bitmap(sched, t->perceived_prio_class);

    if (t->effective_priority == THREAD_PRIO_CLASS_TIMESHARE)
        sched->total_weight += t->weight;

    atomic_inc(&scheduler_data.total_threads);
}

static inline size_t scheduler_get_thread_count(struct scheduler *sched,
                                                enum thread_prio_class prio) {
    return sched->thread_count[prio];
}

static inline struct list_head *
scheduler_get_this_thread_queue(struct scheduler *sched,
                                enum thread_prio_class prio) {
    switch (prio) {
    case THREAD_PRIO_CLASS_URGENT: return &sched->urgent_threads;
    case THREAD_PRIO_CLASS_REALTIME: return &sched->rt_threads;
    case THREAD_PRIO_CLASS_TIMESHARE: return NULL; /* Use the tree */
    case THREAD_PRIO_CLASS_BACKGROUND: return &sched->bg_threads;
    }
    unreachable("invalid thread_prio_class");
}

static inline void enqueue_to_tree(struct scheduler *sched,
                                   struct thread *thread) {
    rbt_insert(&sched->thread_rbt, &thread->rq_tree_node);
}

static inline void retire_thread(struct scheduler *sched,
                                 struct thread *thread) {
    rbt_insert(&sched->completed_rbt, &thread->rq_tree_node);
}

static inline void dequeue_from_tree(struct scheduler *sched,
                                     struct thread *thread) {
    if (rbt_has_node(&sched->completed_rbt, &thread->rq_tree_node))
        return rbt_delete(&sched->completed_rbt, &thread->rq_tree_node);

    rbt_delete(&sched->thread_rbt, &thread->rq_tree_node);
}

/* The `thread_rbt` should be NULL here */
static inline void swap_queues(struct scheduler *sched) {
    enforce(sched->thread_rbt.root == NULL);
    sched->thread_rbt.root = sched->completed_rbt.root;
    sched->completed_rbt.root = NULL;
}

static inline bool scheduler_ts_empty(struct scheduler *sched) {
    return sched->thread_rbt.root == NULL && sched->completed_rbt.root == NULL;
}

static inline enum thread_prio_class
available_prio_level_from_bitmap(uint8_t bitmap) {
    return 31 - ci_clz((uint32_t) bitmap);
}

static inline struct thread *find_highest_prio(struct scheduler *sched) {
    struct rbt_node *node = rbt_max(&sched->thread_rbt);
    if (!node)
        return NULL;

    rbt_delete(&sched->thread_rbt, node);

    return thread_from_rq_rbt_node(node);
}

/* Don't touch `current_period` here */
static inline void disable_period(struct scheduler *sched) {
    sched->period_enabled = false;
    sched->period_ms = 0;
    sched->period_start_ms = 0;
}

static inline bool scheduler_tick_enabled(struct scheduler *sched) {
    return atomic_load(&sched->tick_enabled);
}

static inline bool scheduler_set_tick_enabled(struct scheduler *sched,
                                              bool new) {
    return atomic_exchange(&sched->tick_enabled, new);
}

static inline const char *thread_state_str(const enum thread_state state) {
    switch (state) {
    case THREAD_STATE_IDLE_THREAD: return "IDLE THREAD";
    case THREAD_STATE_READY: return "READY";
    case THREAD_STATE_RUNNING: return "RUNNING";
    case THREAD_STATE_BLOCKED: return "BLOCKED";
    case THREAD_STATE_SLEEPING: return "SLEEPING";
    case THREAD_STATE_ZOMBIE: return "ZOMBIE";
    case THREAD_STATE_TERMINATED: return "TERMINATED";
    case THREAD_STATE_HALTED: return "HALTED";
    }
    unreachable("invalid thread state");
}

static inline const char *
thread_activity_class_str(enum thread_activity_class c) {
    switch (c) {
    case THREAD_ACTIVITY_CLASS_CPU_BOUND: return "CPU BOUND";
    case THREAD_ACTIVITY_CLASS_IO_BOUND: return "IO BOUND";
    case THREAD_ACTIVITY_CLASS_INTERACTIVE: return "INTERACTIVE";
    case THREAD_ACTIVITY_CLASS_SLEEPY: return "SLEEPY";
    case THREAD_ACTIVITY_CLASS_UNKNOWN: return "UNKNOWN";
    }
    unreachable("invalid thread activity class");
}

static inline int64_t thread_virtual_runtime_left(struct thread *t) {
    int64_t ret = t->virtual_budget - t->virtual_period_runtime;
    return MAX(ret, INT64_C(0));
}

static inline void thread_scale_back_delta(struct thread *thread) {
    thread->dynamic_delta = (thread->dynamic_delta * 1000) / 1100;
}

static inline void
scheduler_acquire_two_locks(struct scheduler *a, struct scheduler *b,
                            enum irql *a_irql_out,
                            enum irql *b_irql_out) TSA_NO_ANALYSIS {
    enforce(a != b);
    if (a < b) {
        *a_irql_out = spin_lock_high(&a->lock);
        *b_irql_out = spin_lock_high(&b->lock);
    } else {
        *b_irql_out = spin_lock_high(&b->lock);
        *a_irql_out = spin_lock_high(&a->lock);
    }
}

static inline void
scheduler_release_two_locks(struct scheduler *a, struct scheduler *b,
                            enum irql a_irql,
                            enum irql b_irql) TSA_NO_ANALYSIS {
    if (a == b)
        return spin_unlock(&a->lock, a_irql);

    if (a > b) {
        spin_unlock(&a->lock, a_irql);
        spin_unlock(&b->lock, b_irql);
    } else {
        spin_unlock(&b->lock, b_irql);
        spin_unlock(&a->lock, a_irql);
    }
}

/* this function and the other release_two_raw_locks is only to be used
 * from inside of scheduler_yield() and friends. nowhere else! */
static inline void
scheduler_acquire_two_raw_locks(struct scheduler *a,
                                struct scheduler *b) TSA_NO_ANALYSIS {
    enforce(a != b);
    if (a < b) {
        spin_lock_raw(&a->lock);
        spin_lock_raw(&b->lock);
    } else {
        spin_lock_raw(&b->lock);
        spin_lock_raw(&a->lock);
    }
}

static inline void
scheduler_release_two_raw_locks(struct scheduler *a,
                                struct scheduler *b) TSA_NO_ANALYSIS {
    enforce(a != b);
    if (a > b) {
        spin_unlock_raw(&a->lock);
        spin_unlock_raw(&b->lock);
    } else {
        spin_unlock_raw(&b->lock);
        spin_unlock_raw(&a->lock);
    }
}

static inline void scheduler_defer_resched(struct scheduler *target) {
    cpu_mask_set(&smp_core_scheduler()->deferred_resched_mask, target->core_id);
}

/* Internal use only */
void scheduler_switch_in(void);

/* NOTE: This is merely a place to hook things into.
 * The actual CPU that calls this is undefined */
void thread_post_migrate(struct thread *t, size_t old_cpu, size_t new_cpu);

/* Called with wait_lock held after every object link is removed */
void scheduler_complete_object_wait(struct thread *t,
                                    enum thread_resume_reason reason);

void scheduler_resume_for_apc(struct thread *t);
void thread_class_inc_locked(struct thread *t, enum thread_prio_class class);
void thread_class_dec_locked(struct thread *t, enum thread_prio_class class);
