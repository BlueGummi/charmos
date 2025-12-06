/* @title: Nightmare test framework */
#include <asm.h>
#include <crypto/prng.h>
#include <mem/alloc.h>
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

    void (*write_fn)(struct nightmare_report *r, const char *msg, size_t len);

    uint32_t flags;
};

struct nightmare_test {
    const char *name;
    struct nightmare_watchdog *watchdog;
    time_t default_runtime_ms;
    size_t default_threads;

    _Atomic enum nightmare_state state;
    _Atomic enum nightmare_test_error error;
    struct nightmare_role roles[NIGHTMARE_ROLE_MAX];
    size_t role_count;

    void (*reset)(struct nightmare_test *);
    void (*init)(struct nightmare_test *); /* initialize the test */
    enum nightmare_test_error (*start)(
        struct nightmare_test *);              /* start the test */
    void (*stop)(struct nightmare_test *);     /* stop the test gracefully */
    void (*shutdown)(struct nightmare_test *); /* force it to shutdown */
    void (*report)(struct nightmare_test *,
                   struct nightmare_report *); /* print logs to stdout or
                                                * the input param if not NULL */

    size_t message_count;
    char **messages;
    void *private;
} __linker_aligned;

struct nightmare_local {
    void *data;
    size_t len;
};

struct nightmare_thread {
    _Atomic(struct thread *) th;
    struct nightmare_test *test;
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

#ifdef TEST_NIGHTMARE
static inline bool nightmare_should_stop(void) {
    return atomic_load(&global.nightmare_stop);
}
#endif

static inline void nightmare_chaos_pause() {
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

static inline bool nightmare_watchdog_expired(struct nightmare_watchdog *w,
                                              time_t timeout_ms) {
    time_t now = time_get_ms();
    time_t last = atomic_load(&w->last_progress);

    if (now - last > timeout_ms)
        return true;
    return false;
}

static inline void nightmare_watchdog_init(struct nightmare_watchdog *w) {
    time_t now = time_get_ms();
    atomic_store(&w->last_progress, now);
    w->last_kick_ms = now;
}

static inline void nightmare_set_local(void *d, size_t l) {
    struct nightmare_local *lcl = scheduler_get_current_thread()->private;
    lcl->data = d;
    lcl->len = l;
}

static inline struct nightmare_local *nightmare_get_local() {
    return scheduler_get_current_thread()->private;
}

static inline struct nightmare_thread *nightmare_get_thread() {
    return container_of(nightmare_get_local(), struct nightmare_thread, local);
}

extern struct nightmare_test __skernel_nightmare_tests[];
extern struct nightmare_test __ekernel_nightmare_tests[];

#define NIGHTMARE_THREAD_ENTRY(__name) static void __name(void *__arg)
#define NIGHTMARE_RESET_FN_NAME(__name) __name##_reset
#define NIGHTMARE_INIT_FN_NAME(__name) __name##_init
#define NIGHTMARE_START_FN_NAME(__name) __name##_start
#define NIGHTMARE_STOP_FN_NAME(__name) __name##_stop
#define NIGHTMARE_SHUTDOWN_FN_NAME(__name) __name##_shutdown
#define NIGHTMARE_REPORT_FN_NAME(__name) __name##_report

#define NIGHTMARE_THREAD_ENTRY_INIT()                                          \
    (void) __arg;                                                              \
    struct nightmare_test *SELF = nightmare_get_thread()->test;                \
    (void) SELF;

#define NIGHTMARE_THREAD_ENTRY_EXIT()                                          \
    atomic_store(&nightmare_get_thread()->th, NULL)

#define NIGHTMARE_FN_INIT() (void) SELF;

#define NIGHTMARE_DEFINE_RESET(__name)                                         \
    static void NIGHTMARE_RESET_FN_NAME(__name)(struct nightmare_test * SELF)

#define NIGHTMARE_DEFINE_INIT(__name)                                          \
    static void NIGHTMARE_INIT_FN_NAME(__name)(struct nightmare_test * SELF)

#define NIGHTMARE_DEFINE_START(__name)                                         \
    static enum nightmare_test_error NIGHTMARE_START_FN_NAME(__name)(          \
        struct nightmare_test * SELF)

#define NIGHTMARE_DEFINE_STOP(__name)                                          \
    static void NIGHTMARE_STOP_FN_NAME(__name)(struct nightmare_test * SELF)

#define NIGHTMARE_DEFINE_SHUTDOWN(__name)                                      \
    static void NIGHTMARE_SHUTDOWN_FN_NAME(__name)(struct nightmare_test * SELF)

#define NIGHTMARE_DEFINE_REPORT(__name)                                        \
    static void NIGHTMARE_REPORT_FN_NAME(__name)(                              \
        struct nightmare_test * SELF, struct nightmare_report * REPORT)

#define NIGHTMARE_IMPL_RESET(__name) NIGHTMARE_DEFINE_RESET(__name)
#define NIGHTMARE_IMPL_INIT(__name) NIGHTMARE_DEFINE_INIT(__name)
#define NIGHTMARE_IMPL_START(__name) NIGHTMARE_DEFINE_START(__name)
#define NIGHTMARE_IMPL_STOP(__name) NIGHTMARE_DEFINE_STOP(__name)
#define NIGHTMARE_IMPL_SHUTDOWN(__name) NIGHTMARE_DEFINE_SHUTDOWN(__name)
#define NIGHTMARE_IMPL_REPORT(__name) NIGHTMARE_DEFINE_REPORT(__name)

#define NIGHTMARE_RETURN_ERROR(__err)                                          \
    do {                                                                       \
        atomic_store(&SELF->error, __err);                                     \
        return __err;                                                          \
    } while (0)

#define NIGHTMARE_SET_STATE(__nm, __state)                                     \
    do {                                                                       \
        atomic_store(&__nm->state, __state);                                   \
    } while (0)

#define NIGHTMARE_ASSERT(cond)                                                 \
    do {                                                                       \
        if (!(cond))                                                           \
            NIGHTMARE_RETURN_ERROR(NIGHTMARE_ERR_FAIL);                        \
    } while (0)

#define NIGHTMARE_PROGRESS() nightmare_kick(SELF->watchdog)
#define NIGHTMARE_ASSERT_EQ(a, b) NIGHTMARE_ASSERT((a) == (b))

#define NIGHTMARE_DEFINE_TEST(__name, __runtime_ms, __threads)                 \
    static struct nightmare_test __nightmare_test_##name                       \
        __attribute__((section(".kernel_nightmare_tests"), used)) = {          \
            .name = #__name,                                                   \
            .default_runtime_ms = __runtime_ms,                                \
            .default_threads = __threads,                                      \
            .state = NIGHTMARE_UNINIT,                                         \
            .error = NIGHTMARE_ERR_OK,                                         \
            .roles = {0},                                                      \
            .role_count = 0,                                                   \
            .reset = NIGHTMARE_RESET_FN_NAME(__name),                          \
            .init = NIGHTMARE_INIT_FN_NAME(__name),                            \
            .start = NIGHTMARE_START_FN_NAME(__name),                          \
            .stop = NIGHTMARE_STOP_FN_NAME(__name),                            \
            .shutdown = NIGHTMARE_SHUTDOWN_FN_NAME(__name),                    \
            .report = NIGHTMARE_REPORT_FN_NAME(__name),                        \
    }

#define NIGHTMARE_ADD_MESSAGE(msg)                                             \
    do {                                                                       \
        SELF->messages =                                                       \
            krealloc(SELF->messages, sizeof(char *) * ++SELF->message_count,   \
                     ALLOC_PARAMS_DEFAULT);                                    \
        SELF->messages[SELF->message_count - 1] = msg;                         \
    } while (0)
