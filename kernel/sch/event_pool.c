#include <compiler.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sync/condvar.h>
#include <sync/spin_lock.h>

/* Array of pointers to pools */
static struct event_pool **pools = NULL;
static int64_t num_pools = 0;

/* Basic get functions */
static inline struct event_pool *get_event_pool_local(void) {
    uint64_t core_id = get_this_core_id();
    return pools[core_id];
}

static inline struct thread *worker_spawn_on_core() {
    uint64_t stack_size = PAGE_SIZE;
    struct thread *t = thread_create_custom_stack(worker_main, stack_size);
    if (!t)
        return NULL;

    t->flags = THREAD_FLAGS_NO_STEAL;
    return t;
}

static struct event_pool *get_least_loaded_pool_except_core(int64_t core_num) {
    uint64_t minimum_load = UINT64_MAX;

    if (core_num == -1) {
        /* don't avoid any core */
    }

    /* There will always be a 'core 0 thread' */
    struct event_pool *least_loaded = pools[0];
    for (int64_t i = 0; i < num_pools; i++) {
        if (atomic_load(&pools[i]->num_tasks) < minimum_load && i != core_num) {
            minimum_load = atomic_load(&pools[i]->num_tasks);
            least_loaded = pools[i];
        }
    }

    return least_loaded;
}

static inline bool event_pool_lock(struct event_pool *p) {
    return spin_lock(&p->lock);
}

static inline void event_pool_unlock(struct event_pool *p, bool iflag) {
    spin_unlock(&p->lock, iflag);
}

//
//
// On demand worker thread spawning
//
//

// clang-format off
_Static_assert(MAX_INTERACTIVITY_CHECK_PERIOD / 4 > MIN_INTERACTIVITY_CHECK_PERIOD, "");
// clang-format on

static inline time_t get_inactivity_timeout(struct event_pool *pool) {
    uint32_t num_workers = atomic_load(&pool->num_workers);

    if (num_workers <= (MAX_WORKERS / 8))
        return MAX_INTERACTIVITY_CHECK_PERIOD;

    if (num_workers <= (MAX_WORKERS / 4))
        return MAX_INTERACTIVITY_CHECK_PERIOD / 2;

    if (num_workers <= (MAX_WORKERS / 2))
        return MAX_INTERACTIVITY_CHECK_PERIOD / 4;

    return MIN_INTERACTIVITY_CHECK_PERIOD;
}

static inline uint64_t get_available_worker_idx(struct event_pool *pool) {
    /* start at 1 since index 0 is the permanent one */
    for (uint64_t i = 1; i < MAX_WORKERS; i++)
        if (pool->threads[i].present == false)
            return i;

    k_panic("No space to spawn this worker! State should be unreachable!\n");
}

static inline void link_thread_and_worker(struct worker_thread *worker,
                                          struct thread *thread) {
    worker->present = true;
    worker->timeout_ran = true;
    worker->thread = thread;
    thread->worker = worker;
}

static inline void update_pool_after_spawn(struct event_pool *pool) {
    atomic_fetch_add(&pool->num_workers, 1);
    pool->last_spawn_attempt = time_get_ms();
}

static bool should_spawn_worker(struct event_pool *pool) {
    bool can_spawn = time_get_ms() - pool->last_spawn_attempt > SPAWN_DELAY;
    if (!can_spawn)
        return false;

    bool no_idle_workers = pool->idle_workers == 0;
    bool still_work_to_do = (pool->head - pool->tail) > 0;

    bool not_enough_workers = no_idle_workers && still_work_to_do;
    bool not_exceeded_limit = atomic_load(&pool->num_workers) < MAX_WORKERS;
    bool should_spawn = not_exceeded_limit && not_enough_workers;

    bool currently_not_spawning = !pool->currently_spawning;

    return should_spawn && currently_not_spawning;
}

static bool spawn_worker(struct event_pool *pool) {
    bool i = event_pool_lock(pool);
    struct worker_thread *w = &pool->threads[get_available_worker_idx(pool)];
    w->inactivity_check_period = get_inactivity_timeout(pool);
    struct thread *t = worker_spawn_on_core();

    if (!t) {
        pool->currently_spawning = false;
        event_pool_unlock(pool, i);
        return false;
    }

    link_thread_and_worker(w, t);
    scheduler_enqueue_on_core(t, pool->core);

    update_pool_after_spawn(pool);
    pool->currently_spawning = false;
    event_pool_unlock(pool, i);
    return true;
}

static void spawn_worker_dpc(void *arg1, void *unused) {
    (void) unused;
    struct event_pool *pool = arg1;
    spawn_worker(pool);
}

static bool do_spawn_worker(struct event_pool *pool, bool interrupts) {
    pool->currently_spawning = true;
    event_pool_unlock(pool, interrupts);

    if (!in_interrupt()) {
        return spawn_worker(pool);
    } else {
        return event_pool_add_fast(spawn_worker_dpc, pool, NULL);
    }
}

static inline bool try_spawn_worker(struct event_pool *pool, bool interrupts) {
    if (should_spawn_worker(pool))
        return do_spawn_worker(pool, interrupts);

    return false;
}

static inline struct event_pool *get_least_loaded_pool(void) {
    return get_least_loaded_pool_except_core(-1);
}

static inline struct event_pool *get_least_loaded_remote_pool(void) {
    return get_least_loaded_pool_except_core(get_this_core_id());
}

static inline struct worker_thread *get_this_worker_thread(void) {
    return scheduler_get_curr_thread()->worker;
}

//
//
// Main worker thread loop
//
//

static inline bool signaled_by_timeout(bool signaled) {
    return !signaled;
}

static bool worker_wait(struct event_pool *pool, struct worker_thread *worker,
                        bool interrupts) {
    const time_t timeout = worker->inactivity_check_period;
    bool signal = true;

    pool->idle_workers++;

    if (worker->timeout_ran && !worker->is_permanent) {
        signal = condvar_wait_timeout(&pool->queue_cv, &pool->lock, timeout,
                                      interrupts);
        worker->timeout_ran = false;
    } else {
        signal = condvar_wait(&pool->queue_cv, &pool->lock, interrupts);
    }

    pool->idle_workers--;

    if (signaled_by_timeout(signal))
        worker->timeout_ran = true;

    return signal;
}

static inline bool worker_should_exit(const struct worker_thread *worker,
                                      time_t start_idle, bool idle,
                                      bool signal) {
    const time_t timeout = worker->inactivity_check_period;
    if (!worker->is_permanent && idle && signaled_by_timeout(signal))
        if (time_get_ms() - start_idle >= timeout)
            return true;

    return false;
}

static inline void set_idle(const struct worker_thread *worker,
                            time_t *start_idle, bool *idle) {
    if (!worker->is_permanent && !(*idle)) {
        *start_idle = time_get_ms();
        *idle = true;
    }
}

static bool idle_loop_should_exit(struct event_pool *pool,
                                  struct worker_thread *worker, bool *idle,
                                  time_t *start_idle, bool interrupts) {
    while (pool->head == pool->tail) {
        set_idle(worker, start_idle, idle);
        bool signal = worker_wait(pool, worker, interrupts);

        if (worker_should_exit(worker, *start_idle, *idle, signal))
            return true;
    }
    return false;
}

static inline struct worker_task pool_dequeue(struct event_pool *pool,
                                              bool interrupts) {
    struct worker_task task = pool->tasks[pool->tail % EVENT_POOL_CAPACITY];
    pool->tail++;
    atomic_fetch_sub(&pool->num_tasks, 1);
    event_pool_unlock(pool, interrupts);
    return task;
}

static void do_work_from_pool(struct event_pool *pool,
                              struct worker_thread *worker, bool *idle,
                              bool interrupts) {
    struct worker_task task = pool_dequeue(pool, interrupts);
    worker->last_active = time_get_ms();
    *idle = false;

    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    task.func(task.arg, task.arg2);
    irql_lower(old);
}

static void worker_exit(struct event_pool *pool, struct worker_thread *worker,
                        bool interrupts) {
    worker->should_exit = true;
    worker->present = false;
    atomic_fetch_sub(&pool->num_workers, 1);
    event_pool_unlock(pool, interrupts);
    thread_exit();
}

void worker_main(void) {
    struct worker_thread *worker = get_this_worker_thread();
    struct event_pool *pool = get_event_pool_local();

    bool interrupts = true;
    while (1) {
        bool idle = false;
        time_t start_idle = 0;

        interrupts = event_pool_lock(pool);

        if (idle_loop_should_exit(pool, worker, &idle, &start_idle, interrupts))
            break;

        do_work_from_pool(pool, worker, &idle, interrupts);
    }

    worker_exit(pool, worker, interrupts);
}

//
//
// Public Interfaces
//
//

static bool add(struct event_pool *pool, dpc_t func, void *arg, void *arg2) {
    bool interrupts = event_pool_lock(pool);

    uint64_t next_head = pool->head + 1;
    if ((next_head - pool->tail) > EVENT_POOL_CAPACITY) {

        /* panic for now - just so we know this went wrong */
        k_panic("Event pool OVERFLOW!\n");
        event_pool_unlock(pool, interrupts);
        return false;
    }

    pool->tasks[pool->head % EVENT_POOL_CAPACITY].func = func;
    pool->tasks[pool->head % EVENT_POOL_CAPACITY].arg = arg;
    pool->tasks[pool->head % EVENT_POOL_CAPACITY].arg2 = arg2;
    pool->head = next_head;
    atomic_fetch_add(&pool->num_tasks, 1);

    try_spawn_worker(pool, interrupts);

    event_pool_unlock(pool, interrupts);
    condvar_signal(&pool->queue_cv);
    return true;
}

static void spawn_permanent_thread_on_core(uint64_t core) {
    struct event_pool *pool = pools[core];
    pool->core = core;

    struct thread *thread = worker_spawn_on_core();
    if (!thread) {
        k_panic("Failed to spawn permanent worker thread on core %llu\n", core);
    }

    struct worker_thread *worker = &pool->threads[0];
    worker->is_permanent = true;
    worker->inactivity_check_period = MINUTES_TO_MS(5);
    link_thread_and_worker(worker, thread);
    scheduler_enqueue_on_core(thread, core);
    update_pool_after_spawn(pool);
}

void event_pool_init(void) {
    num_pools = global.core_count;
    pools = kzalloc(sizeof(struct event_pool *) * num_pools);

    if (!pools)
        k_panic("Failed to allocate space for event pools!\n");

    for (int64_t i = 0; i < num_pools; ++i) {
        pools[i] = kzalloc(sizeof(struct event_pool));
        if (!pools[i])
            k_panic("Failed to allocate space for event pool %u!\n", i);

        uint64_t core_id = i;
        spawn_permanent_thread_on_core(core_id);
    }
}

bool event_pool_add(dpc_t func, void *arg, void *arg2) {
    struct event_pool *pool = get_least_loaded_pool();
    return add(pool, func, arg, arg2);
}

bool event_pool_add_remote(dpc_t func, void *arg, void *arg2) {
    struct event_pool *pool = get_least_loaded_remote_pool();
    return add(pool, func, arg, arg2);
}

bool event_pool_add_local(dpc_t func, void *arg, void *arg2) {
    struct event_pool *pool = get_event_pool_local();
    return add(pool, func, arg, arg2);
}

bool event_pool_add_fast(dpc_t func, void *arg, void *arg2) {
    struct event_pool *pool =
        pools[(get_this_core_id() + 1) % global.core_count];
    return add(pool, func, arg, arg2);
}
