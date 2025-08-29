#include <compiler.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(event_pool, lock);
/* Array of pointers to pools */
static struct event_pool **pools = NULL;
static int64_t num_pools = 0;

/* Basic get functions */
static inline struct event_pool *get_event_pool_local(void) {
    uint64_t core_id = get_this_core_id();
    return pools[core_id];
}

static inline struct thread *worker_create() {
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

static int reserve_slot(struct event_pool *pool) {
    for (uint64_t i = 1; i < MAX_WORKERS; ++i) {
        uint64_t bit = 1ull << i;
        uint64_t old =
            atomic_load_explicit(&pool->worker_bitmap, memory_order_relaxed);

        if (old & bit)
            continue; /* already used */
        if (atomic_compare_exchange_weak_explicit(
                &pool->worker_bitmap, &old, old | bit, memory_order_acq_rel,
                memory_order_relaxed)) {
            return (int) i;
        }
        /* retry - someone else raced */
    }

    k_panic("Unreachable, all worker slots in use\n");
    return -1;
}

static void unreserve_slot(struct event_pool *pool, int idx) {
    uint64_t bit = ~(1ull << idx);
    atomic_fetch_and_explicit(&pool->worker_bitmap, bit, memory_order_acq_rel);
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

static bool claim_spawner(struct event_pool *p) {
    return atomic_flag_test_and_set_explicit(&p->spawner_flag,
                                             memory_order_acq_rel) == 0;
}

static void release_spawner(struct event_pool *p) {
    atomic_flag_clear_explicit(&p->spawner_flag, memory_order_release);
}

static bool spawn_worker(struct event_pool *pool) {
    if (!claim_spawner(pool))
        return false;

    int slot = reserve_slot(pool);
    if (slot < 0) {
        release_spawner(pool);
        return false;
    }

    struct worker_thread *w = &pool->threads[slot];
    w->inactivity_check_period = get_inactivity_timeout(pool);

    struct thread *t = worker_create();
    if (!t) {
        unreserve_slot(pool, slot);
        release_spawner(pool);
        return false;
    }

    w->thread = t;
    w->present = true;
    w->timeout_ran = true;
    t->worker = w;

    atomic_fetch_add(&pool->num_workers, 1);
    atomic_fetch_add(&pool->total_spawned, 1);
    pool->last_spawn_attempt = time_get_ms();

    scheduler_enqueue_on_core(t, pool->core);

    release_spawner(pool);

    return true;
}

static bool should_spawn_worker(struct event_pool *pool) {
    time_t now = time_get_ms();
    if (now - pool->last_spawn_attempt <= SPAWN_DELAY)
        return false;

    bool no_idle = atomic_load(&pool->idle_workers) == 0;
    bool work_pending = atomic_load(&pool->head) != atomic_load(&pool->tail);

    bool under_limit = atomic_load(&pool->num_workers) < MAX_WORKERS;
    bool not_spawning = !atomic_flag_test_and_set(&pool->spawner_flag);

    return no_idle && work_pending && under_limit && not_spawning;
}

static bool try_spawn_worker(struct event_pool *pool) {
    if (!should_spawn_worker(pool))
        return false;

    if (in_interrupt()) {
        atomic_store(&pool->spawn_pending, true);
        return true;
    }

    return spawn_worker(pool);
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

static bool worker_wait(struct event_pool *pool, struct worker_thread *w,
                        enum irql irql) {
    bool signal;
    pool->idle_workers++;

    if (w->timeout_ran && !w->is_permanent) {
        signal = condvar_wait_timeout(&pool->queue_cv, &pool->lock,
                                      w->inactivity_check_period, irql);
        w->timeout_ran = false;

    } else {
        signal = condvar_wait(&pool->queue_cv, &pool->lock, irql);
    }

    pool->idle_workers--;

    if (signaled_by_timeout(signal)) {
        w->timeout_ran = true;
        if (!w->idle) {
            w->idle = true;
            w->start_idle = time_get_ms();
        }
    }

    return signal;
}

static inline bool worker_should_exit(const struct worker_thread *worker,
                                      bool signal) {
    const time_t timeout = worker->inactivity_check_period;
    if (!worker->is_permanent && worker->idle && signaled_by_timeout(signal))
        if (time_get_ms() - worker->start_idle >= timeout)
            return true;

    return false;
}

static bool dequeue_task(struct event_pool *pool, struct worker_task *out) {
    uint64_t pos;
    struct slot *s;

    while (1) {
        pos = atomic_load_explicit(&pool->tail, memory_order_relaxed);
        s = &pool->tasks[pos % EVENT_POOL_CAPACITY];
        uint64_t seq = atomic_load_explicit(&s->seq, memory_order_acquire);
        int64_t diff = (int64_t) seq - (int64_t) (pos + 1);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &pool->tail, &pos, pos + 1, memory_order_acq_rel,
                    memory_order_relaxed)) {

                *out = s->task;
                atomic_store_explicit(&s->seq, pos + EVENT_POOL_CAPACITY,
                                      memory_order_release);
                return true;
            }

            continue;
        } else if (diff < 0) {
            return false;
        }
        cpu_relax();
    }
}

static void worker_exit(struct event_pool *pool, struct worker_thread *worker,

                        enum irql irql) {
    worker->present = false;
    worker->idle = false;
    worker->should_exit = true;

    worker->thread = NULL;

    int slot_idx = (int) (worker - &pool->threads[0]);
    unreserve_slot(pool, slot_idx);

    atomic_fetch_sub(&pool->num_workers, 1);

    event_pool_unlock(pool, irql);

    thread_exit();
}

void worker_main(void) {
    struct worker_thread *w = get_this_worker_thread();
    struct event_pool *pool = get_event_pool_local();

    while (1) {

        struct worker_task task;
        if (dequeue_task(pool, &task)) {
            w->last_active = time_get_ms();
            w->idle = false;

            enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
            task.func(task.arg, task.arg2);
            irql_lower(old);
            continue;
        }

        enum irql irql = event_pool_lock_irq_disable(pool);

        while (atomic_load(&pool->head) == atomic_load(&pool->tail)) {
            if (atomic_load(&pool->spawn_pending)) {
                atomic_store(&pool->spawn_pending, false);
                spawn_worker(pool);
            }

            bool signaled = worker_wait(pool, w, irql);

            if (worker_should_exit(w, signaled))
                worker_exit(pool, w, irql);
        }

        event_pool_unlock(pool, irql);
    }
}

//
//
// Public Interfaces
//
//

static bool enqueue_task(struct event_pool *pool, dpc_t func, void *arg,
                         void *arg2) {
    uint64_t pos;
    struct slot *s;

    while (1) {

        pos = atomic_load_explicit(&pool->head, memory_order_relaxed);
        s = &pool->tasks[pos % EVENT_POOL_CAPACITY];
        uint64_t seq = atomic_load_explicit(&s->seq, memory_order_acquire);
        int64_t diff = (int64_t) seq - (int64_t) pos;

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &pool->head, &pos, pos + 1, memory_order_acq_rel,
                    memory_order_relaxed)) {

                s->task = (struct worker_task) {
                    .func = func, .arg = arg, .arg2 = arg2};

                atomic_store_explicit(&s->seq, pos + 1, memory_order_release);
                condvar_signal(&pool->queue_cv);
                try_spawn_worker(pool);
                return true;
            }
            continue;
        } else if (diff < 0) {
            k_panic("Event pool overflow\n");
            return false;
        }
        cpu_relax();
    }
}

static void spawn_permanent_thread_on_core(uint64_t core) {
    struct event_pool *pool = pools[core];
    pool->core = core;

    struct thread *thread = worker_create();
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
            k_panic("Failed to allocate space for event pool %ld!\n", i);

        spinlock_init(&pools[i]->lock);

        for (uint64_t j = 0; j < EVENT_POOL_CAPACITY; ++j)
            atomic_store_explicit(&pools[i]->tasks[j].seq, j,
                                  memory_order_relaxed);

        uint64_t core_id = i;
        spawn_permanent_thread_on_core(core_id);
    }
}

bool event_pool_add(dpc_t func, void *arg, void *arg2) {
    struct event_pool *pool = get_least_loaded_pool();
    return enqueue_task(pool, func, arg, arg2);
}

bool event_pool_add_remote(dpc_t func, void *arg, void *arg2) {
    struct event_pool *pool = get_least_loaded_remote_pool();
    return enqueue_task(pool, func, arg, arg2);
}

bool event_pool_add_local(dpc_t func, void *arg, void *arg2) {
    struct event_pool *pool = get_event_pool_local();
    return enqueue_task(pool, func, arg, arg2);
}

bool event_pool_add_fast(dpc_t func, void *arg, void *arg2) {
    struct event_pool *pool =
        pools[(get_this_core_id() + 1) % global.core_count];
    return enqueue_task(pool, func, arg, arg2);
}
