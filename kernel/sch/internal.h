#include <kassert.h>
#include <sch/sched.h>

static inline void scheduler_set_queue_bitmap(struct scheduler *sched,
                                              uint8_t prio) {
    atomic_fetch_or(&sched->queue_bitmap, 1 << prio);
}

static inline void scheduler_clear_queue_bitmap(struct scheduler *sched,
                                                uint8_t prio) {
    atomic_fetch_and(&sched->queue_bitmap, ~(1 << prio));
}

static inline uint8_t scheduler_get_bitmap(struct scheduler *sched) {
    return atomic_load(&sched->queue_bitmap);
}

static inline struct scheduler *smp_core_scheduler(void) {
    return global.schedulers[smp_core_id()];
}

static inline struct idle_thread_data *smp_core_idle_thread(void) {
    return &smp_core_scheduler()->idle_thread_data;
}

static inline bool thread_exhausted_period(struct scheduler *sched,
                                           struct thread *thread) {
    return thread->completed_period == sched->current_period;
}

static inline void scheduler_decrement_thread_count(struct scheduler *sched,
                                                    struct thread *t) {
    sched->total_thread_count--;
    sched->thread_count[t->perceived_prio_class]--;

    if (sched->thread_count[t->perceived_prio_class] == 0)
        scheduler_clear_queue_bitmap(sched, t->perceived_prio_class);

    if (t->effective_priority == THREAD_PRIO_CLASS_TIMESHARE)
        sched->total_weight -= t->weight;

    atomic_fetch_sub(&scheduler_data.total_threads, 1);
}

static inline void scheduler_increment_thread_count(struct scheduler *sched,
                                                    struct thread *t) {
    sched->total_thread_count++;
    sched->thread_count[t->perceived_prio_class]++;
    scheduler_set_queue_bitmap(sched, t->perceived_prio_class);

    if (t->effective_priority == THREAD_PRIO_CLASS_TIMESHARE)
        sched->total_weight += t->weight;

    atomic_fetch_add(&scheduler_data.total_threads, 1);
}

static inline struct list_head *
scheduler_get_this_thread_queue(struct scheduler *sched,
                                enum thread_prio_class prio) {
    switch (prio) {
    case THREAD_PRIO_CLASS_URGENT: return &sched->urgent_threads;
    case THREAD_PRIO_CLASS_RT: return &sched->rt_threads;
    case THREAD_PRIO_CLASS_TIMESHARE: return NULL; /* Use the tree */
    case THREAD_PRIO_CLASS_BACKGROUND: return &sched->bg_threads;
    }
    kassert_unreachable();
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
        return rb_delete(&sched->completed_rbt, &thread->rq_tree_node);

    rb_delete(&sched->thread_rbt, &thread->rq_tree_node);
}

/* The `thread_rbt` should be NULL here */
static inline void swap_queues(struct scheduler *sched) {
    kassert(sched->thread_rbt.root == NULL);
    sched->thread_rbt.root = sched->completed_rbt.root;
    sched->completed_rbt.root = NULL;
}

static inline bool scheduler_ts_empty(struct scheduler *sched) {
    return sched->thread_rbt.root == NULL && sched->completed_rbt.root == NULL;
}

static inline struct thread *find_highest_prio(struct scheduler *sched) {
    struct rbt_node *node = rbt_max(&sched->thread_rbt);
    if (!node)
        return NULL;

    rb_delete(&sched->thread_rbt, node);

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
    kassert_unreachable();
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
    kassert_unreachable();
}

static inline int64_t thread_virtual_runtime_left(struct thread *t) {
    int64_t ret = t->virtual_budget - t->virtual_period_runtime;
    return ret < 0 ? 0 : ret;
}

static inline void thread_scale_back_delta(struct thread *thread) {
    thread->dynamic_delta = (thread->dynamic_delta * 1000) / 1100;
}

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(scheduler, lock);

/* Internal use only */
void thread_wake_locked(struct thread *t, enum thread_wake_reason r,
                        void *wake_src);
void scheduler_drop_locks_after_switch_in();
