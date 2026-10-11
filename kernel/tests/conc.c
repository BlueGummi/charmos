#include <sch/sched.h>
#include <smp/core.h>
#include <test/conc.h>
#include <thread/thread.h>
#include <thread/wait.h>
#include <time/time.h>

#if defined(TEST_ENABLED) || defined(TEST_NIGHTMARE_ENABLED)

PERCPU_DEFINE(struct test_progress_counter, test_progress_store);
PERCPU_EXPORT_AS(test_progress, test_progress_store);

uint64_t test_conc_progress_sum(void) {
    uint64_t sum = 0;
    struct test_progress_counter *counter;
    percpu_for_each(counter, test_progress) {
        sum += atomic_load_rlx(&counter->count);
    }

    return sum;
}

uint64_t test_rng_seed_for(uint64_t base_seed, size_t index) {
    struct test_rng rng = {.state = base_seed ^
                                    (PRNG_SPLITMIX64_GAMMA * (index + 1))};
    return test_rng_next(&rng);
}

void test_conc_init(struct test_conc *c, struct test_conc_worker *workers,
                    size_t worker_count) {
    atomic_store_rlx(&c->stop, TEST_RUN);
    atomic_store_rlx(&c->quiesce_requested, false);
    atomic_store_rlx(&c->parked_count, 0);
    atomic_store_rlx(&c->aux, NULL);
    c->workers = workers;
    c->worker_count = worker_count;
    completion_init(&c->start, COMPLETION_INIT_NORMAL);
    atomic_store_release(&c->active, false);
}

void test_conc_publish_stop(struct test_conc *c, enum test_stop reason) {
    enum test_stop observed = atomic_load_acq(&c->stop);
    bool advanced = false;

    while (observed < reason) {
        if (atomic_cas_weak(&c->stop, &observed, reason, mo_acq_rel,
                            mo_acquire)) {
            advanced = true;
            break;
        }
    }

    if (!advanced || observed != TEST_RUN)
        return;

    for (size_t i = 0; i < c->worker_count; i++) {
        struct thread *thread = atomic_load_acq(&c->workers[i].th);

        if (thread)
            thread_alert(thread);
    }

    struct thread *aux = atomic_load_acq(&c->aux);
    if (aux)
        thread_alert(aux);
}

void test_conc_park(struct test_conc *c, struct test_conc_worker *w) {
    bool was_parked = atomic_xchg_acq_rel(&w->parked, true);
    if (!was_parked)
        atomic_inc_release(&c->parked_count);

    while (test_conc_must_park(c) && !test_conc_must_stop(c))
        scheduler_yield();

    if (!was_parked)
        test_conc_unpark_self(c, w);
}

void test_conc_unpark_self(struct test_conc *c, struct test_conc_worker *w) {
    if (!atomic_load_acq(&w->parked))
        return;

    atomic_dec_release(&c->parked_count);
    atomic_store_release(&w->parked, false);
}

static bool test_liveness_eval(struct test_liveness_state *state) {
    if (atomic_load_acq(&state->phase) != TEST_LIVE_ARMED)
        return false;

    bool quiesce = test_conc_must_park(state->conc);
    uint64_t current_progress = test_conc_progress_sum();
    time_ms_t now_ms = time_get_ms();
    if (quiesce) {
        state->last_progress = current_progress;
        state->last_change_ms = now_ms;
        state->was_quiesced = true;
        return false;
    }

    if (state->was_quiesced) {
        state->last_progress = current_progress;
        state->last_change_ms = now_ms;
        state->was_quiesced = false;
        return false;
    }

    if (current_progress != state->last_progress) {
        state->last_progress = current_progress;
        state->last_change_ms = now_ms;
        return false;
    }

    if (now_ms < state->last_change_ms ||
        now_ms - state->last_change_ms < state->threshold_ms)
        return false;

    state->pending = (struct test_stall_evidence){
        .silent_ms = now_ms - state->last_change_ms,
        .progress = current_progress,
    };

    enum test_liveness_phase expected = TEST_LIVE_ARMED;
    if (!atomic_cas_strong(&state->phase, &expected, TEST_LIVE_PENDING,
                           mo_acq_rel, mo_acquire))
        return false;

    return true;
}

static void test_liveness_watchdog_cb(struct watchdog_callback *cb) {
    struct test_liveness_state *state =
        container_of(cb, struct test_liveness_state, callback);
    struct test_conc *c = state->conc;

    if (!c)
        return;

    if (!atomic_load_acq(&c->active))
        return;

    if (test_conc_stop_reason(c) != TEST_RUN)
        return;

    if (test_liveness_eval(state))
        test_conc_publish_stop(c, TEST_STOP_STALL);
}

bool test_liveness_start(struct test_liveness_state *state,
                         struct test_conc *conc, time_ms_t threshold_ms,
                         enum test_on_stall policy) {
    *state = (struct test_liveness_state){
        .callback = {.fn = test_liveness_watchdog_cb},
        .phase = ATOMIC_VAR_INIT(TEST_LIVE_ARMED),
        .last_progress = test_conc_progress_sum(),
        .last_change_ms = time_get_ms(),
        .threshold_ms = threshold_ms,
        .was_quiesced = false,
        .policy = policy,
        .coordinator_cpu = 0,
        .registered = false,
        .conc = conc,
    };

    watchdog_callback_add(state->coordinator_cpu, &state->callback);
    state->registered = true;
    return true;
}

void test_liveness_stop(struct test_liveness_state *state) {
    if (state->registered) {
        watchdog_callback_remove(state->coordinator_cpu, &state->callback);
        state->registered = false;
    }

    atomic_store_release(&state->phase, TEST_LIVE_OFF);
}

bool test_liveness_take(struct test_liveness_state *state,
                        struct test_stall_evidence *out) {
    enum test_liveness_phase expected = TEST_LIVE_PENDING;
    if (!atomic_cas_strong(&state->phase, &expected, TEST_LIVE_REPORTED,
                           mo_acq_rel, mo_acquire))
        return false;

    if (out)
        *out = state->pending;
    return true;
}

#endif /* TEST_ENABLED || TEST_NIGHTMARE_ENABLED */
