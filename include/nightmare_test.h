/* @title: Nightmare test framework */
#include <asm.h>
#include <crypto/prng.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <types/types.h>

enum nightmare_role_type {
    NIGHTMARE_ROLE_GENERIC,
    NIGHTMARE_ROLE_SLEEPER,
    NIGHTMARE_ROLE_WAKER,
    NIGHTMARE_ROLE_MIGRATOR,
    NIGHTMARE_ROLE_APC_SPAMMER,
    NIGHTMARE_ROLE_FORKER,
    NIGHTMARE_ROLE_ALLOCATOR,
    NIGHTMARE_ROLE_INVALIDATOR,
    NIGHTMARE_ROLE_MAX
};

enum nightmare_test_error {
    NIGHTMARE_ERR_OK,    /* test OK */
    NIGHTMARE_ERR_FAIL,  /* test did not succeed */
    NIGHTMARE_ERR_RETRY, /* test should be retried from the top */
    NIGHTMARE_ERR_PANIC, /* test panicked */
};

enum nightmare_state {
    NIGHTMARE_UNINIT,  /* default state, not initialized */
    NIGHTMARE_READY,   /* ready to run */
    NIGHTMARE_RUNNING, /* running */
    NIGHTMARE_STOPPED, /* test stopped */
};

struct nightmare_role {
    enum nightmare_role_type type;
    const char *name;

    /* number of threads for this role */
    size_t count;

    /* the worker function each spawned thread runs */
    void (*worker)(void *);
    void *arg;
};

struct nightmare_watchdog {
    atomic_uint last_progress;
    time_t last_kick_ms;
};

struct nightmare_report {
    char *buffer;
    size_t buffer_len;

    void (*write_fn)(void *ctx, const char *msg, size_t len);
    void *write_ctx;

    uint32_t flags;
};

struct nightmare_test {
    const char *name;
    time_t default_runtime_ms;
    size_t default_threads;

    _Atomic enum nightmare_state state;
    struct nightmare_role roles[NIGHTMARE_ROLE_MAX];
    size_t role_count;

    void (*reset)(struct nightmare_test *);
    void (*init)(struct nightmare_test *);     /* initialize the test */
    enum nightmare_test_error (*start)(void);  /* start the test */
    void (*stop)(struct nightmare_test *);     /* stop the test gracefully */
    void (*shutdown)(struct nightmare_test *); /* force it to shutdown */
    void (*report)(struct nightmare_test *,
                   struct nightmare_report *); /* print logs to stdout or
                                                * the input param if not NULL */

    void *private;
} __attribute__((aligned(64)));

struct nightmare_local {
    void *data;
    size_t len;
};

struct nightmare_thread {
    struct thread *th;
    enum nightmare_role_type role;
    struct nightmare_local local;
};

struct nightmare_thread_group {
    struct nightmare_thread *threads;
    size_t count;
};

struct nightmare {
    struct nightmare_test *test;
    struct nightmare_thread *self;
    struct nightmare_watchdog *watchdog;
};

static inline void nightmare_add_role(struct nightmare_test *t,
                                      enum nightmare_role_type type,
                                      const char *name, void (*worker)(void *),
                                      size_t count, void *arg) {
    size_t idx = t->role_count++;
    t->roles[idx].type = type;
    t->roles[idx].name = name;
    t->roles[idx].worker = worker;
    t->roles[idx].count = count;
    t->roles[idx].arg = arg;
}

static inline bool nightmare_should_stop(void) {
    return atomic_load(&global.nightmare_stop);
}

static inline void chaos_pause() {
    int r = prng_next() % 5;
    switch (r) {
    case 0: cpu_relax(); break;
    case 1: scheduler_yield(); break;
    default: break;
    }
}

static inline void nightmare_kick(struct nightmare_watchdog *w) {
    atomic_store(&w->last_progress, time_get_ms());
}

void nightmare_spawn_roles(struct nightmare_test *,
                           struct nightmare_thread_group *);
void nightmare_join_roles(struct nightmare_thread_group *);
enum nightmare_test_error nightmare_run(struct nightmare_test *t);
bool nightmare_watchdog_expired(struct nightmare_watchdog *, time_t timeout_ms);
void nightmare_watchdog_init(struct nightmare_watchdog *);
void nightmare_set_local(struct nightmare_local *, void *, size_t);
void *nightmare_get_local(struct nightmare_local *);

#define DEFINE_NIGHTMARE_TEST(name)                                            \
    static struct nightmare_test name                                          \
        __attribute__((section(".kernel_nightmare_tests")))
