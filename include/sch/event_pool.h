#include <sch/defer.h>
#include <spin_lock.h>
#include <sch/condvar.h>

struct worker_task {
    defer_func_t func;
    void *arg;
    struct worker_task *next;
};

struct event_pool {
    struct worker_task *head;
    struct worker_task *tail;
    struct spinlock lock;
    struct condvar queue_cv;
};

void event_pool_init(uint64_t num_threads);

/* can only fail from allocation fail */
bool event_pool_add(defer_func_t func, void *arg);
