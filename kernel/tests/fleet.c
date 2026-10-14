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

struct test_fleet_alloc {
    struct test_fleet_alloc *next;
    /* payload */
};

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

static struct test_conc_worker *test_fleet_claim(struct test_fleet *f,
                                                 const char *role,
                                                 test_worker_fn body,
                                                 void *arg) {
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

struct test_conc_worker *test_fleet_spawn(struct test_fleet *f,
                                          const char *role, test_worker_fn body,
                                          void *arg) {
    struct test_conc_worker *w = test_fleet_claim(f, role, body, arg);
    if (!w)
        return NULL;

    struct thread *t = thread_spawn(
        ("test_%s_%zu", role, w->index), test_fleet_worker_main, .arg = w,
        .joinable = true, .flags = f->opts.pin ? THREAD_FLAG_PINNED : 0);

    return test_fleet_attach(f, w, t) ? w : NULL;
}

struct test_conc_worker *test_fleet_spawn_on_core(struct test_fleet *f,
                                                  const char *role,
                                                  test_worker_fn body,
                                                  void *arg, cpu_id_t core_id) {
    struct test_conc_worker *w = test_fleet_claim(f, role, body, arg);
    if (!w)
        return NULL;

    struct thread *t = thread_create(
        ("test_%s_%zu", role, w->index), test_fleet_worker_main, .arg = w,
        .joinable = true, .allowed_cpus = cpu_mask_of(core_id),
        .flags = THREAD_FLAG_PINNED);
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

    test_err("  %s[%zu] '%s': state=%d flags=0x%x wait_type=%d parked=%d",
             w->role ? w->role : "worker", w->index, t->name,
             (int) thread_get_state(t), (unsigned) thread_get_flags(t),
             (int) thread_get_wait_type(t),
             (int) atomic_load(&((struct test_conc_worker *) w)->parked));

    test_err("  %s[%zu]   apcs_pending=0x%x apcs_ran=%u ctxsw=%zu",
             w->role ? w->role : "worker", w->index,
             (unsigned) atomic_load(&t->apc_pending_mask), t->total_apcs_ran,
             t->context_switches);

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

static void test_fleet_drain(struct test_fleet *f, enum test_stop reason) {
    test_conc_publish_stop(&f->conc, reason);
    test_fleet_arm_hard_deadline(f);

    for (size_t i = 0; i < f->count; i++) {
        struct thread *t = atomic_load_acq(&f->workers[i].th);

        if (t)
            thread_join(t);
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

void *test_fleet_alloc(struct test_fleet *f, size_t size) {
    struct test_fleet_alloc *a = valloc(sizeof(*a) + size, ALLOC_ZERO);
    if (!a)
        return NULL;

    a->next = f->allocs;
    f->allocs = a;
    return (void *) (a + 1);
}

static void test_fleet_free_allocs(struct test_fleet *f) {
    struct test_fleet_alloc *a = f->allocs;
    while (a) {
        struct test_fleet_alloc *next = a->next;
        vfree(a);
        a = next;
    }
    f->allocs = NULL;
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

    test_fleet_free_allocs(f);
    vfree(f);
    return leaked;
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
