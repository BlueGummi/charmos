#include <sch/defer.h>
#include <sync/condvar.h>
#include <sync/spin_lock.h>

struct worker_task {
    dpc_t func;
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
bool event_pool_add(dpc_t func, void *arg);
