#include <compiler.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sync/condvar.h>
#include <sync/spin_lock.h>

static struct event_pool pool = {0};

static void worker_main(void) {
    while (1) {
        bool interrupts = spin_lock(&pool.lock);

        while (pool.head == pool.tail)
            condvar_wait(&pool.queue_cv, &pool.lock);

        struct worker_task task = pool.tasks[pool.tail % EVENT_POOL_CAPACITY];
        pool.tail++;

        spin_unlock(&pool.lock, interrupts);

        task.func(task.arg);
    }
}

bool event_pool_add(dpc_t func, void *arg) {
    bool interrupts = spin_lock(&pool.lock);

    uint64_t next_head = pool.head + 1;
    if ((next_head - pool.tail) > EVENT_POOL_CAPACITY) {

        /* panic for now */
        k_panic("Event pool OVERFLOW!\n");
        spin_unlock(&pool.lock, interrupts);
        return false;
    }

    pool.tasks[pool.head % EVENT_POOL_CAPACITY].func = func;
    pool.tasks[pool.head % EVENT_POOL_CAPACITY].arg = arg;
    pool.head = next_head;

    condvar_signal(&pool.queue_cv);
    spin_unlock(&pool.lock, interrupts);
    return true;
}

void event_pool_init(uint64_t num_threads) {
    spinlock_init(&pool.lock);
    condvar_init(&pool.queue_cv);
    pool.head = pool.tail = 0;

    for (uint64_t i = 0; i < num_threads; ++i)
        thread_spawn(worker_main);
}
