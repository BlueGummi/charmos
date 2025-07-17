#include <mem/alloc.h>
#include <sch/event_pool.h>

static struct event_pool pool = {0};

static void worker_main(void) {
    while (1) {
        bool interrupts = spin_lock(&pool.lock);

        while (pool.head == NULL) {
            condvar_wait(&pool.queue_cv, &pool.lock);
        }

        struct worker_task *task = pool.head;
        pool.head = task->next;
        if (!pool.head)
            pool.tail = NULL;

        spin_unlock(&pool.lock, interrupts);

        task->func(task->arg);
        kfree(task);
    }
}

void event_pool_add(defer_func_t func, void *arg) {
    struct worker_task *task = kmalloc(sizeof(*task));
    task->func = func;
    task->arg = arg;
    task->next = NULL;

    bool interrupts = spin_lock(&pool.lock);
    if (pool.tail) {
        pool.tail->next = task;
        pool.tail = task;
    } else {
        pool.head = pool.tail = task;
    }

    condvar_signal(&pool.queue_cv);
    spin_unlock(&pool.lock, interrupts);
}

void event_pool_init(uint64_t num_threads) {
    spinlock_init(&pool.lock);
    condvar_init(&pool.queue_cv);

    for (uint64_t i = 0; i < num_threads; ++i)
        thread_spawn(worker_main);
}
