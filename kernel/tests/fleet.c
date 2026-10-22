#include <console/panic.h>
#include <container_of.h>
#include <global.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdarg.h>
#include <string.h>
#include <test/fleet.h>
#include <thread/thread.h>
#include <thread/thread_diag.h>
#include <time/time.h>

#ifdef TEST_ENABLED

cpu_id_t test_core(size_t index) {
    size_t count = global.core_count ? global.core_count : 1;
    return (cpu_id_t) (index % count);
}

void *test_alloc(struct test_context *ctx, size_t size) {
    if (!ctx)
        return NULL;

    struct test_alloc_node *a = valloc(sizeof(*a) + size, ALLOC_ZERO);
    if (!a)
        return NULL;

    a->next = ctx->allocs;
    ctx->allocs = a;
    return (void *) (a + 1);
}

static void test_alloc_teardown(struct test_context *ctx) {
    if (!ctx)
        return;

    struct test_alloc_node *a = ctx->allocs;
    while (a) {
        struct test_alloc_node *next = a->next;
        vfree(a);
        a = next;
    }
    ctx->allocs = NULL;
}

void *test_fleet_alloc(struct test_fleet *f, size_t size) {
    return (f && f->ctx) ? test_alloc(f->ctx, size) : NULL;
}

struct thread *test_spawn_internal(struct test_context *ctx,
                                   void (*entry)(void *),
                                   const struct thread_create_params *params,
                                   const char *name) {
    if (!ctx)
        return NULL;

    if (ctx->helper_count >= TEST_MAX_HELPERS) {
        test_err("test_spawn: exceeded TEST_MAX_HELPERS (%d)",
                 TEST_MAX_HELPERS);
        return NULL;
    }

    struct thread_create_params p =
        params ? *params : (struct thread_create_params){0};
    p.joinable = true;

    struct thread *t = thread_spawn_internal(entry, p, "%s", name);
    if (!t)
        return NULL;

    enforce(thread_getref(t));
    ctx->helpers[ctx->helper_count++] = t;
    return t;
}

static bool test_helpers_teardown(struct test_context *ctx) {
    if (!ctx || ctx->helper_count == 0)
        return false;

    bool leaked = false;
    for (size_t i = 0; i < ctx->helper_count; i++) {
        struct thread *t = ctx->helpers[i];
        if (!t)
            continue;
        ctx->helpers[i] = NULL;
        if (!(thread_get_flags(t) & THREAD_FLAG_JOINED)) {
            leaked = true;
            thread_alert(t);
            thread_join(t);
        }
        thread_putref(t);
    }
    ctx->helper_count = 0;
    return leaked;
}

struct test_run_on_core_ctx {
    void (*fn)(void *);
    void *arg;
};

void test_fleet_report(struct test_fleet *f, const char *file, uint32_t line,
                       const char *fmt, ...) {

    if (!once_token_claim(&f->failed)) {
        test_fleet_stop(f, TEST_STOP_FINDING);
        return;
    }

    f->fail_file = file;
    f->fail_line = line;

    if (f->ctx) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(f->ctx->fail_msg, (int) sizeof(f->ctx->fail_msg), fmt, args);
        va_end(args);
    }

    test_fleet_stop(f, TEST_STOP_FINDING);
}

/* We first wait for the completion to bother us, then run */
static void test_fleet_worker_main(void *arg) {
    struct test_conc_worker *self = arg;

    struct test_conc_worker *base = self - self->index;
    struct test_fleet *f = container_of(base, struct test_fleet, workers[0]);

    completion_wait(&f->conc.start);
    atomic_inc_release(&f->started);

    if (!test_conc_must_stop(&f->conc)) {
        test_worker_fn body = f->bodies[self->index];
        if (body)
            body(f, self);
    }

    test_conc_unpark_self(&f->conc, self);
    atomic_inc_release(&f->finished);
}

static void test_fleet_deadline(struct timer *timer) {
    struct test_fleet *f = container_of(timer, struct test_fleet, hard_timer);

    test_conc_publish_stop(&f->conc, TEST_STOP_STALL);
    panic("test fleet did not drain within deadline");
}

struct test_fleet *test_fleet_init(struct test_context *ctx,
                                   const struct test_fleet_opts *opts) {
    struct test_fleet *f = vnew(f, ALLOC_ZERO);
    if (!f)
        return NULL;

    once_token_init(&f->failed);
    f->ctx = ctx;
    f->opts = opts ? *opts : (struct test_fleet_opts){0};

    if (!f->opts.soft_ms)
        f->opts.soft_ms = TEST_FLEET_SOFT_MS;

    if (!f->opts.drain_ms)
        f->opts.drain_ms = TEST_FLEET_DRAIN_MS;

    test_conc_init(&f->conc, f->workers, 0);

    if (ctx)
        ctx->fleet = f;

    return f;
}

static bool test_fleet_perturb_body(struct test_fleet *f,
                                    struct test_conc_worker *self);

static struct test_conc_worker *test_fleet_claim(struct test_fleet *f,
                                                 const char *role,
                                                 test_worker_fn body,
                                                 void *arg) {
    /* Perturbers target workers[0, subject_count) */
    enforce(!f->has_perturbers || body == test_fleet_perturb_body,
            "fleet subjects must be spawned before perturbers");

    if (f->count >= TEST_FLEET_MAX_WORKERS) {
        f->spawn_failed = true;
        return NULL;
    }

    struct test_conc_worker *w = &f->workers[f->count];
    w->index = f->count;
    w->role = role;
    w->arg = arg;
    w->rng.state = test_rng_seed_for(f->ctx ? f->ctx->seed : 0, w->index);
    f->bodies[f->count] = body;

    f->count++;
    f->conc.worker_count = f->count;
    return w;
}

static bool test_fleet_attach(struct test_fleet *f, struct test_conc_worker *w,
                              struct thread *t) {
    if (!t) {
        f->spawn_failed = true;
        f->count--;
        f->conc.worker_count = f->count;
        return false;
    }

    enforce(thread_getref(t));
    atomic_store_release(&w->th, t);
    return true;
}

struct test_conc_worker *
test_fleet_spawn_opts(struct test_fleet *f, const char *role,
                      test_worker_fn body,
                      const struct test_fleet_worker_opts *opts) {
    void *arg = opts ? opts->arg : NULL;
    struct test_conc_worker *w = test_fleet_claim(f, role, body, arg);
    if (!w)
        return NULL;

    enum thread_flags flags = f->opts.pin ? THREAD_FLAG_PINNED : 0;
    enum thread_prio_class prio_class = THREAD_PRIO_CLASS_TIMESHARE;
    cpu_id_t core_id = CPU_ID_MAX;

    if (opts) {
        if (opts->flags)
            flags |= opts->flags;
        if (opts->prio_class)
            prio_class = opts->prio_class;
        core_id = opts->core_id;
    }

    if (core_id != CPU_ID_MAX) {
        struct thread *t = thread_create(
            ("test_%s_%zu", role, w->index), test_fleet_worker_main, .arg = w,
            .joinable = true, .allowed_cpus = cpu_mask_of(core_id),
            .flags = flags | THREAD_FLAG_PINNED, .base_prio_class = prio_class);
        if (!t) {
            f->spawn_failed = true;
            f->count--;
            f->conc.worker_count = f->count;
            return NULL;
        }

        enforce(thread_getref(t));
        atomic_store_release(&w->th, t);
        thread_enqueue_on_core(t, core_id);
        return w;
    }

    struct thread *t = thread_spawn(
        ("test_%s_%zu", role, w->index), test_fleet_worker_main, .arg = w,
        .joinable = true, .flags = flags, .base_prio_class = prio_class);

    return test_fleet_attach(f, w, t) ? w : NULL;
}

struct test_conc_worker *test_fleet_spawn(struct test_fleet *f,
                                          const char *role, test_worker_fn body,
                                          void *arg) {
    return test_fleet_spawn_opts(f, role, body,
                                 &(struct test_fleet_worker_opts){
                                     .arg = arg,
                                     .core_id = CPU_ID_MAX,
                                 });
}

struct test_conc_worker *test_fleet_spawn_on_core(struct test_fleet *f,
                                                  const char *role,
                                                  test_worker_fn body,
                                                  void *arg, cpu_id_t core_id) {
    return test_fleet_spawn_opts(f, role, body,
                                 &(struct test_fleet_worker_opts){
                                     .arg = arg,
                                     .core_id = core_id,
                                 });
}

struct thread *test_fleet_worker_thread(struct test_fleet *f,
                                        const char *role) {
    if (!f || !role)
        return NULL;

    for (size_t i = 0; i < f->count; i++) {
        if (f->workers[i].role && strcmp(f->workers[i].role, role) == 0)
            return atomic_load_acq(&f->workers[i].th);
    }

    return NULL;
}

size_t test_fleet_spawn_per_core(struct test_fleet *f, const char *role,
                                 test_worker_fn body, void *arg,
                                 size_t per_core) {
    size_t spawned = 0;

    for (size_t rep = 0; rep < per_core; rep++) {
        for (size_t cpu = 0; cpu < global.core_count; cpu++) {
            if (!test_fleet_spawn_on_core(f, role, body, arg, cpu))
                return spawned;
            spawned++;
        }
    }

    return spawned;
}

static bool test_fleet_perturb_body(struct test_fleet *f,
                                    struct test_conc_worker *self) {
    const struct test_perturb_desc *desc = self->arg;
    desc->fn(&f->perturb_env, self);
    return true;
}

size_t test_fleet_spawn_perturbers(struct test_fleet *f,
                                   const char *const *names,
                                   int64_t victim_cpu) {
    if (!f->has_perturbers) {
        /* One victim for the whole fleet */
        if (victim_cpu == TEST_PERTURB_VICTIM_ANY) {
            uint64_t seed = test_rng_seed_for(f->ctx ? f->ctx->seed : 0,
                                              TEST_FLEET_MAX_WORKERS);
            victim_cpu = (int64_t) (seed % global.core_count);
        }

        f->subject_count = f->count;
        f->perturb_env = (struct test_perturb_env){
            .conc = &f->conc,
            .subject_count = f->subject_count,
            .victim_cpu = victim_cpu,
        };
        f->has_perturbers = true;
    }

    size_t spawned = 0;
    for (const char *const *name = names; *name; name++) {
        const struct test_perturb_desc *desc = test_perturb_lookup(*name);
        enforce(desc, "no such perturber");

        if (!test_fleet_spawn(f, desc->name, test_fleet_perturb_body,
                              (void *) desc))
            break;
        spawned++;
    }

    return spawned;
}

void test_fleet_start_all(struct test_fleet *f) {
    atomic_store_release(&f->conc.active, true);

    if (f->opts.soft_ms)
        test_liveness_start(&f->liveness, &f->conc, f->opts.soft_ms,
                            TEST_ON_STALL_REPORT);

    complete_all(&f->conc.start);
}

#define TEST_FLEET_DUMP_ARMS 8

static void test_fleet_dump_worker(const struct test_conc_worker *w) {
    struct thread *t = atomic_load_acq(&w->th);

    if (!t) {
        test_err("  %s[%zu]: <not spawned>", w->role ? w->role : "worker",
                 w->index);
        return;
    }

    struct scheduler *rq = atomic_load_acq(&t->scheduler);

    test_err("  %s[%zu] '%s': state=%d flags=0x%x wait_type=%d parked=%d "
             "rq=%ld",
             w->role ? w->role : "worker", w->index, t->name,
             (int) thread_get_state(t), (unsigned) thread_get_flags(t),
             (int) thread_get_wait_type(t),
             (int) atomic_load(&((struct test_conc_worker *) w)->parked),
             rq ? (long) rq->core_id : -1L);

    if (thread_get_state(t) == THREAD_STATE_ZOMBIE)
        return;

#ifdef DEBUG_THREAD
    test_err("  %s[%zu]   apcs_pending=0x%x apcs_ran=%u ctxsw=%zu",
             w->role ? w->role : "worker", w->index,
             (unsigned) atomic_load(&t->apc_pending_mask), t->total_apcs_ran,
             t->context_switches);
#endif

    thread_dump_wait_trace(t, w->role ? w->role : "worker", w->index,
                           TEST_FLEET_DUMP_ARMS);
}

static void test_fleet_dump(struct test_fleet *f,
                            const struct test_stall_evidence *ev) {
    if (ev)
        test_err("fleet stalled: silent_ms=%lu progress=%lu",
                 (unsigned long) ev->silent_ms, (unsigned long) ev->progress);

    test_err("fleet: %zu worker(s), started=%zu finished=%zu stop=%s", f->count,
             atomic_load(&f->started), atomic_load(&f->finished),
             test_stop_to_str(atomic_load(&f->conc.stop)));

    if (f->has_perturbers)
        test_err("fleet: %zu subject(s), perturbers from [%zu], victim cpu %ld",
                 f->subject_count, f->subject_count,
                 (long) f->perturb_env.victim_cpu);

    for (size_t i = 0; i < f->count; i++)
        test_fleet_dump_worker(&f->workers[i]);
}

static void test_fleet_arm_hard_deadline(struct test_fleet *f) {
    timer_init(&f->hard_timer, test_fleet_deadline, NULL);
    f->hard_timer.flags = TIMER_FLAG_IRQ;
    f->hard_timer.expiration_us = time_get_us() + MS_TO_US(f->opts.drain_ms);
    timer_add_global(&f->hard_timer);
    f->hard_armed = true;
}

static void test_fleet_disarm_hard_deadline(struct test_fleet *f) {
    if (f->hard_armed) {
        timer_shutdown_sync(&f->hard_timer);
        f->hard_armed = false;
    }
}

static void test_fleet_join_range(struct test_fleet *f, size_t from,
                                  size_t to) {
    for (size_t i = from; i < to; i++) {
        struct thread *t = atomic_load_acq(&f->workers[i].th);

        if (t)
            thread_join(t);
    }
}

static void test_fleet_drain(struct test_fleet *f, enum test_stop reason) {
    test_conc_publish_stop(&f->conc, reason);
    test_fleet_arm_hard_deadline(f);

    size_t subjects = f->has_perturbers ? f->subject_count : f->count;
    test_fleet_join_range(f, 0, subjects);

    /* Perturbers run until told to stop */
    if (subjects < f->count) {
        test_conc_publish_stop(&f->conc, TEST_STOP_BUDGET);
        test_fleet_join_range(f, subjects, f->count);
    }

    test_fleet_disarm_hard_deadline(f);
}

static void test_fleet_thread_putref(struct test_fleet *f) {
    for (size_t i = 0; i < f->count; i++) {
        struct thread *t = atomic_xchg_acq_rel(&f->workers[i].th, NULL);
        if (t)
            thread_putref(t);
    }

    atomic_store_release(&f->conc.active, false);
}

struct test_verdict test_fleet_join(struct test_fleet *f) {
    if (!atomic_load_acq(&f->conc.active))
        test_fleet_start_all(f);

    bool spawn_failed = f->spawn_failed;

    test_fleet_drain(f, f->opts.run_until_stop ? TEST_STOP_BUDGET : TEST_RUN);

    struct test_stall_evidence ev;
    bool stalled = test_liveness_take(&f->liveness, &ev);
    test_liveness_stop(&f->liveness);

    bool failed = once_token_claimed(&f->failed);
    const char *fail_file = f->fail_file;
    uint32_t fail_line = f->fail_line;

    if (stalled || failed || spawn_failed)
        test_fleet_dump(f, stalled ? &ev : NULL);

    test_fleet_thread_putref(f);
    f->joined = true;

    if (spawn_failed)
        return TEST_FAIL("fleet could not spawn all workers");

    if (failed) {
        test_err("worker failure at %s:%u: %s", fail_file ? fail_file : "?",
                 fail_line, f->ctx ? f->ctx->fail_msg : "");
        return TEST_FAIL(f->ctx ? f->ctx->fail_msg : "worker failed");
    }

    if (stalled)
        return TEST_FAIL("fleet stalled: no progress before the soft deadline");

    return TEST_SUCCESS;
}

bool test_fleet_teardown(struct test_context *ctx) {
    struct test_fleet *f = ctx->fleet;
    if (!f)
        return false;

    ctx->fleet = NULL;

    bool leaked = !f->joined && f->count > 0;
    if (leaked) {
        if (!atomic_load_acq(&f->conc.active))
            test_fleet_start_all(f);

        test_fleet_drain(f, TEST_STOP_BUDGET);
        test_liveness_stop(&f->liveness);
        test_fleet_thread_putref(f);
    }

    vfree(f);
    return leaked;
}

bool test_context_teardown(struct test_context *ctx) {
    if (!ctx)
        return false;

    bool leaked_fleet = test_fleet_teardown(ctx);
    bool leaked_helpers = test_helpers_teardown(ctx);
    test_alloc_teardown(ctx);
    return leaked_fleet || leaked_helpers;
}

static void test_run_on_core_main(void *arg) {
    struct test_run_on_core_ctx *c = arg;
    c->fn(c->arg);
}

/* TODO: there is probably a nicer way to go about a "bother random
 * CPU to run arbitrary pinned code in thread context and wait for
 * it to finish". We have system workqueues per-CPU, but we have no
 * flush_work() equivalent. Once we get that, this will be much
 * cleaner and can just be a workqueue call wrapper */
bool test_run_on_core(cpu_id_t core_id, void (*fn)(void *), void *arg) {
    struct test_run_on_core_ctx c = {.fn = fn, .arg = arg};

    struct thread *t = thread_spawn(
        ("test_on_core_%zu", core_id), test_run_on_core_main, .arg = &c,
        .joinable = true, .allowed_cpus = cpu_mask_of(core_id),
        .flags = THREAD_FLAG_PINNED, .on_cpu = core_id);
    if (!t)
        return false;

    thread_join(t);
    return true;
}

#endif /* TEST_ENABLED */
