/* @title: Nightmare test framework */
#include <stdatomic.h>
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

struct nightmare_test {
    const char *name;
    time_t default_runtime_ms;
    size_t default_threads;

    _Atomic enum nightmare_state state;
    struct nightmare_role roles[NIGHTMARE_ROLE_MAX];
    size_t role_count;

    void (*reset)(struct nightmare_test *);
    void (*init)(struct nightmare_test *);    /* initialize the test */
    enum nightmare_test_error (*start)(void); /* start the test */
    void (*stop)(struct nightmare_test *);    /* stop the test gracefully */

    void (*force_shutdown)(struct nightmare_test *); /* force it to shutdown */
    void (*report)(char *);                          /* print logs to stdout or
                                                      * the input param if not NULL */

    void *private;
} __attribute__((aligned(64)));

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
