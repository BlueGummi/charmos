#include <sch/defer.h>

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(workqueue, lock);

extern int64_t num_workqueues;
extern struct workqueue **workqueues;

static inline struct workqueue *workqueue_local(void) {
    uint64_t core_id = get_this_core_id();
    return workqueues[core_id];
}

static inline struct thread *worker_create_unmigratable() {
    uint64_t stack_size = PAGE_SIZE;
    struct thread *t = thread_create_custom_stack(worker_main, stack_size);
    if (!t)
        return NULL;

    t->flags = THREAD_FLAGS_NO_STEAL;
    return t;
}

static inline struct worker_thread *get_this_worker_thread(void) {
    return scheduler_get_curr_thread()->worker;
}

static inline bool workqueue_empty(struct workqueue *queue) {
    return atomic_load(&queue->head) == atomic_load(&queue->tail);
}

static inline void workqueue_set_needs_spawn(struct workqueue *queue,
                                             bool needs) {
    atomic_store(&queue->spawn_pending, needs);
}

static inline bool workqueue_needs_spawn(struct workqueue *queue) {
    return atomic_load(&queue->spawn_pending);
}

bool workqueue_try_spawn_worker(struct workqueue *queue);
bool workqueue_dequeue_task(struct workqueue *queue, struct worker_task *out);
bool workqueue_enqueue_task(struct workqueue *queue, dpc_t func, void *arg,
                            void *arg2);
void workqueue_link_thread_and_worker(struct worker_thread *worker,
                                      struct thread *thread);
void workqueue_update_queue_after_spawn(struct workqueue *queue);
bool workqueue_spawn_worker(struct workqueue *queue);
int workqueue_reserve_slot(struct workqueue *queue);
void workqueue_unreserve_slot(struct workqueue *queue, int idx);
struct workqueue *workqueue_least_loaded_queue_except(int64_t except_core_num);
struct workqueue *workqueue_get_least_loaded(void);
struct workqueue *workqueue_get_least_loaded_remote(void);
