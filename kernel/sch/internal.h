#include <kassert.h>
#include <sch/sched.h>

static inline struct scheduler *smp_core_scheduler(void) {
    return global.schedulers[smp_core_id()];
}

static inline struct idle_thread_data *smp_core_idle_thread(void) {
    return &smp_core_scheduler()->idle_thread_data;
}

static inline void scheduler_decrement_thread_count(struct scheduler *sched,
                                                    struct thread *t) {
    sched->total_thread_count--;
    sched->thread_count[t->perceived_priority]--;
    atomic_fetch_sub(&scheduler_data.total_threads, 1);
}

static inline void scheduler_increment_thread_count(struct scheduler *sched,
                                                    struct thread *t) {
    sched->total_thread_count++;
    sched->thread_count[t->perceived_priority]++;
    atomic_fetch_add(&scheduler_data.total_threads, 1);
}

static inline struct thread_queue *
scheduler_get_this_thread_queue(struct scheduler *sched,
                                enum thread_prio_class prio) {
    switch (prio) {
    case THREAD_PRIO_CLASS_URGENT: return &sched->urgent_threads;
    case THREAD_PRIO_CLASS_RT: return &sched->rt_threads;
    case THREAD_PRIO_CLASS_TIMESHARE: return NULL; /* Use the tree */
    case THREAD_PRIO_CLASS_BACKGROUND: return &sched->bg_threads;
    }
    k_panic("unreachable!\n");
}

static inline void enqueue_to_tree(struct scheduler *sched,
                                   struct thread *thread) {
    rbt_insert(&sched->thread_rbt, &thread->tree_node);
}

static inline void retire_thread(struct scheduler *sched,
                                 struct thread *thread) {
    rbt_insert(&sched->completed_rbt, &thread->tree_node);
}

/* The `thread_rbt` should be NULL here */
static inline void swap_queues(struct scheduler *sched) {
    kassert(sched->thread_rbt.root == NULL);
    sched->thread_rbt.root = sched->completed_rbt.root;
    sched->completed_rbt.root = NULL;
}

static inline struct thread *find_highest_prio(struct scheduler *sched,
                                               enum thread_prio_class prio) {
    struct rbt_node *node = rbt_max(&sched->thread_rbt);
    if (!node)
        return NULL;

    rb_delete(&sched->thread_rbt, node);
    if (sched->thread_rbt.root == NULL && sched->completed_rbt.root == NULL)
        atomic_fetch_and(&sched->queue_bitmap, ~(1 << prio));

    return thread_from_rbt_node(node);
}

static inline bool thread_exhausted_period(struct scheduler *sched,
                                           struct thread *thread) {
    return thread->completed_period == sched->current_period;
}

/* Don't touch `current_period` here */
static inline void disable_period(struct scheduler *sched) {
    sched->period_enabled = false;
    sched->period_ms = 0;
    sched->period_start_ms = 0;
}

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

static inline bool scheduler_tick_enabled(struct scheduler *sched) {
    return atomic_load(&sched->tick_enabled);
}

static inline bool scheduler_set_tick_enabled(struct scheduler *sched,
                                              bool new) {
    return atomic_exchange(&sched->tick_enabled, new);
}

static inline const char *thread_state_str(const enum thread_state state) {
    switch (state) {
    case THREAD_STATE_NEW: return "NEW";
    case THREAD_STATE_IDLE_THREAD: return "IDLE THREAD";
    case THREAD_STATE_READY: return "READY";
    case THREAD_STATE_RUNNING: return "RUNNING";
    case THREAD_STATE_BLOCKED: return "BLOCKED";
    case THREAD_STATE_SLEEPING: return "SLEEPING";
    case THREAD_STATE_ZOMBIE: return "ZOMBIE";
    case THREAD_STATE_TERMINATED: return "TERMINATED";
    case THREAD_STATE_HALTED: return "HALTED";
    }
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
}

static inline const char *thread_event_reason_str(const uint8_t reason) {
    switch (reason) {
    case THREAD_WAKE_REASON_BLOCKING_IO: return "WAKE FROM BLOCKING IO";
    case THREAD_WAKE_REASON_BLOCKING_MANUAL: return "WAKE FROM BLOCKING WAKE";
    case THREAD_WAKE_REASON_SLEEP_TIMEOUT: return "WAKE FROM SLEEP TIMEOUT";
    case THREAD_WAKE_REASON_SLEEP_MANUAL: return "WAKE FROM MANUAL SLEEP WAKE";
    case THREAD_WAKE_REASON_UNKNOWN: return "WAKE FROM UNKNOWN REASON";
    case THREAD_BLOCK_REASON_IO: return "BLOCK FROM IO";
    case THREAD_BLOCK_REASON_MANUAL: return "BLOCK FROM MANUAL BLOCK";
    case THREAD_BLOCK_REASON_UNKNOWN: return "BLOCK FROM UNKNOWN REASON";
    case THREAD_SLEEP_REASON_MANUAL: return "SLEEP FROM MANUAL SLEEP";
    case THREAD_SLEEP_REASON_UNKNOWN: return "SLEEP FROM UNKNOWN REASON";
    default: return "UNKNOWN EVENT REASON";
    }
}

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(scheduler, lock);
