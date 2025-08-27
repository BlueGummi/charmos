#include <kassert.h>
#include <sch/sched.h>

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

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(scheduler, lock);
