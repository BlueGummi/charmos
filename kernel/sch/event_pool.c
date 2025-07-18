#include <compiler.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sync/condvar.h>
#include <sync/spin_lock.h>

/* TODO: Many threads per core */
static struct event_pool **pools = NULL;
static int64_t num_pools = 0;

static struct event_pool *get_event_pool_local(void) {
    uint64_t core_id = get_sch_core_id();
    return pools[core_id];
}

static struct event_pool *get_least_loaded_pool_except_core(int64_t core_num) {
    uint64_t minimum_load = UINT64_MAX;

    /* nothing - this just means that it won't avoid any core */
    if (core_num == -1)
        ;

    /* There will always be a 'core 0 thread' */
    struct event_pool *least_loaded = pools[0];
    for (int64_t i = 0; i < num_pools; i++) {
        if (pools[i]->num_tasks < minimum_load && i != core_num) {
            minimum_load = pools[i]->num_tasks;
            least_loaded = pools[i];
        }
    }

    return least_loaded;
}

static struct event_pool *get_least_loaded_pool(void) {
    return get_least_loaded_pool_except_core(-1);
}

static struct event_pool *get_least_loaded_remote_pool(void) {
    return get_least_loaded_pool_except_core(get_sch_core_id());
}

static void worker_main(void) {
    while (1) {
        struct event_pool *pool = get_event_pool_local();
        bool interrupts = spin_lock(&pool->lock);

        while (pool->head == pool->tail)
            condvar_wait(&pool->queue_cv, &pool->lock);

        struct worker_task task = pool->tasks[pool->tail % EVENT_POOL_CAPACITY];
        pool->tail++;
        pool->num_tasks--;

        spin_unlock(&pool->lock, interrupts);

        task.func(task.arg);
    }
}

static bool add(struct event_pool *pool, dpc_t func, void *arg) {
    bool interrupts = spin_lock(&pool->lock);

    uint64_t next_head = pool->head + 1;
    if ((next_head - pool->tail) > EVENT_POOL_CAPACITY) {

        /* panic for now - just so we know this went wrong */
        k_panic("Event pool OVERFLOW!\n");
        spin_unlock(&pool->lock, interrupts);
        return false;
    }

    pool->tasks[pool->head % EVENT_POOL_CAPACITY].func = func;
    pool->tasks[pool->head % EVENT_POOL_CAPACITY].arg = arg;
    pool->head = next_head;
    pool->num_tasks++;

    condvar_signal(&pool->queue_cv);
    spin_unlock(&pool->lock, interrupts);
    return true;
}

bool event_pool_add(dpc_t func, void *arg) {
    struct event_pool *pool = get_least_loaded_pool();
    return add(pool, func, arg);
}

bool event_pool_add_remote(dpc_t func, void *arg) {
    struct event_pool *pool = get_least_loaded_remote_pool();
    return add(pool, func, arg);
}

bool event_pool_add_local(dpc_t func, void *arg) {
    struct event_pool *pool = get_event_pool_local();
    return add(pool, func, arg);
}

void event_pool_init(uint64_t num_threads) {
    num_pools = num_threads;
    pools = kzalloc(sizeof(struct event_pool *) * num_pools);

    if (!pools)
        k_panic("Failed to allocate space for event pools!\n");

    for (uint64_t i = 0; i < num_threads; ++i) {
        pools[i] = kzalloc(sizeof(struct event_pool));
        if (!pools[i])
            k_panic("Failed to allocate space for event pool %u!\n", i);

        uint64_t core_count = scheduler_get_core_count();
        struct thread *t = thread_spawn_on_core(worker_main, i % core_count);

        /* We don't want these migrated across cores */
        t->flags = NO_STEAL;
    }
}
