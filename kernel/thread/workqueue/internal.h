#include <stdarg.h>
#include <thread/defer.h>

#define DEQUEUE_FROM_ONESHOT_CODE 2
#define DEQUEUE_FROM_REGULAR_CODE 1

#define worklist_from_worklist_list_node(node)                                 \
    container_of(node, struct worklist, worklist_list)

#define work_from_worklist_node(node) container_of(node, struct work, list_node)

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(workqueue, lock);
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(worklist, lock);

#define WORKQUEUE_CORE_UNBOUND (-1)

#define WORKQUEUE_NUM_WORKS(wq) (atomic_load(&wq->num_tasks))

static inline uint64_t workqueue_current_worker_count(struct workqueue *q) {
    return atomic_load(&q->num_workers);
}

static inline bool workqueue_works_empty(struct workqueue *queue) {
    enum irql irql = spin_lock(&queue->work_lock);
    bool empty = list_empty(&queue->works);
    spin_unlock(&queue->work_lock, irql);
    return empty;
}

static inline bool workqueue_empty(struct workqueue *queue) {
    return atomic_load(&queue->num_tasks) == 0;
}

static inline void workqueue_set_needs_spawn(struct workqueue *queue,
                                             bool needs) {
    atomic_store(&queue->spawn_pending, needs);
}

static inline bool workqueue_needs_spawn(struct workqueue *queue) {
    return atomic_load(&queue->spawn_pending);
}

static inline bool workqueue_get(struct workqueue *queue) {
    return refcount_inc(&queue->refcount);
}

static inline void workqueue_put(struct workqueue *queue) {
    if (refcount_dec_and_test(&queue->refcount))
        return workqueue_free(queue);
}

static inline bool worklist_get(struct worklist *wlist) {
    return refcount_inc(&wlist->refcount);
}

static inline void worklist_put(struct worklist *wlist) {
    if (refcount_dec_and_test(&wlist->refcount))
        worklist_free(wlist);
}

/* Regardless of if we use static workers or not this will
 * always be the list of workers we have */
static inline void workqueue_add_worker(struct workqueue *wq,
                                        struct worker *wker) {
    enum irql irql = spin_lock(&wq->worker_lock);
    list_add(&wker->list_node, &wq->workers);
    spin_unlock(&wq->worker_lock, irql);
}

static inline void workqueue_remove_worker(struct workqueue *wq,
                                           struct worker *worker) {
    enum irql irql = spin_lock(&wq->worker_lock);
    list_del_init(&worker->list_node);
    spin_unlock(&wq->worker_lock, irql);
}

static inline bool ignore_timeouts(struct workqueue *q) {
    return atomic_load(&q->ignore_timeouts);
}

static inline bool workqueue_usable(struct workqueue *q) {
    return atomic_load(&q->state) == WORKQUEUE_STATE_ACTIVE;
}

static inline size_t workqueue_workers(struct workqueue *wq) {
    return atomic_load(&wq->num_workers);
}

static inline size_t workqueue_idlers(struct workqueue *wq) {
    return atomic_load(&wq->idle_workers);
}

bool workqueue_try_spawn_worker(struct workqueue *queue);
int32_t workqueue_dequeue_task(struct workqueue *queue, struct work **out,
                               struct work *oneshot_out);
void workqueue_link_thread_and_worker(struct worker *worker,
                                      struct thread *thread);
bool workqueue_spawn_worker_internal(struct workqueue *queue);
struct workqueue *workqueue_least_loaded_queue_except(int64_t except_core_num);
struct workqueue *workqueue_get_least_loaded(void);
struct workqueue *workqueue_get_least_loaded_remote(void);
struct worker *workqueue_spawn_permanent_worker(struct workqueue *queue,
                                                int64_t core);
struct workqueue *workqueue_create_internal(struct workqueue_attributes *attrs,
                                            const char *fmt, va_list args);
enum thread_request_decision workqueue_request_callback(struct thread *t,
                                                        void *data);
struct thread *worker_create(struct cpu_mask mask, nice_t niceness);
