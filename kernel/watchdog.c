#include <acpi/lapic.h>
#include <cmdline.h>
#include <irq/irq.h>
#include <mem/alloc_or_die.h>
#include <pit.h>
#include <smp/percpu.h>
#include <string.h>
#include <sync/seqlock.h>
#include <time/time.h>
#include <watchdog.h>

#include "watchdog/internal.h"

#define watchdog_master_log(lvl, fmt, ...)                                     \
    log(LOG_SITE(watchdog_master), LOG_HANDLE(watchdog_master), lvl, fmt,      \
        ##__VA_ARGS__)

#define watchdog_master_err(fmt, ...)                                          \
    watchdog_master_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define watchdog_master_warn(fmt, ...)                                         \
    watchdog_master_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define watchdog_master_info(fmt, ...)                                         \
    watchdog_master_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define watchdog_master_debug(fmt, ...)                                        \
    watchdog_master_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define watchdog_master_trace(fmt, ...)                                        \
    watchdog_master_log(LOG_TRACE, fmt, ##__VA_ARGS__)

static void watchdog_worker_timer_func(struct timer *t);
static void watchdog_buckets_init(struct watchdog_buckets *b);
static void watchdog_percpu_ctor(struct watchdog_percpu *pcpu, cpu_id_t cpu);

static struct watchdog_config config = {
    .bucket_interval = SECONDS_TO_NS(1),
    .master_tick_interval = MS_TO_NS(100),
    .worker_heartbeat_interval = MS_TO_NS(50),
    .master_print_interval = MS_TO_NS(250),
    .master_stall_timeout = MS_TO_NS(WATCHDOG_STALL_TIMEOUT_DEFAULT_MS),

    .master_panic_score = FX(0.9), /* We missed 60% of heartbeats for a while,
                                    * AND when tested, we failed 75% */
    .master_critical_score = FX(0.6), /* We're missing 60% of our heartbeats */
    .master_suspect_score = FX(0.25), /* We're missing 25% of our heartbeats */

};
static struct watchdog_master watchdog_master = {0};
static struct watchdog_globals watchdog_global = {0};

/* TODO: a LOG_SITE + HANDLE DECLARE, with printing explicitly turned off
 * because we cannot take the implicit printf lock(s) from the NMI */
LOG_SITE_DECLARE(watchdog_master);
LOG_HANDLE_DECLARE(watchdog_master);

PERCPU_DECLARE(watchdog_percpu, struct watchdog_percpu, watchdog_percpu_ctor);
static CMDLINE_DECLARE(watchdog, .flags = CMDLINE_ENTRY_SYMBOLIC,
                       .desc = "Watchdog command line namespace");

CMDLINE_CHILD_DECLARE(watchdog, master, .flags = CMDLINE_ENTRY_SYMBOLIC);

CMDLINE_CHILDREN_DECLARE(
    CMDLINE_NODE(watchdog, master),
    CMDLINE_INNER_DURATION(heartbeat_interval, config.master_tick_interval,
                           .range = RANGE(MS_TO_NS(1), SECONDS_TO_NS(60))),
    CMDLINE_INNER_DURATION(bucket_interval, config.bucket_interval,
                           .range = RANGE(MS_TO_NS(1), SECONDS_TO_NS(60))),
    CMDLINE_INNER_FX(panic_score, config.master_panic_score,
                     .range = RANGE(0, FX_ONE)),
    CMDLINE_INNER_FX(warn_score, config.master_critical_score,
                     .range = RANGE(0, FX_ONE)),
    CMDLINE_INNER_FX(suspect_threshold, config.master_suspect_score,
                     .range = RANGE(0, FX_ONE)),
    CMDLINE_INNER_DURATION(stall_timeout, config.master_stall_timeout,
                           .range = RANGE(0, SECONDS_TO_NS(600))));

/* PERCPU_DECLARE zero-initializes, but we explicitly init buckets */
static void watchdog_percpu_ctor(struct watchdog_percpu *pcpu, cpu_id_t cpu) {
    pcpu->id = cpu;
    pcpu->pets_enabled = false;
    pcpu->pets = 0;
    pcpu->anti_pets = 0;
    watchdog_buckets_init(&pcpu->buckets);
    pcpu->timer.func = watchdog_worker_timer_func;
    pcpu->timer.flags =
        TIMER_FLAG_PINNED | TIMER_FLAG_CPU(cpu) | TIMER_FLAG_IRQ;
}

static inline size_t time_to_bucket(time_ms_t time) {
    /* TODO: Decide if this condition is not permitted? */
    if (unlikely(watchdog_global.bucket_interval_ms == 0))
        return 0;

    return time / watchdog_global.bucket_interval_ms;
}

static void watchdog_buckets_init(struct watchdog_buckets *b) {
    seqcount_init(&b->seq);
    b->idx = 0;
    b->curr_epoch = 0;
    b->last_heartbeat_ms = 0;
    for (size_t i = 0; i < WATCHDOG_NUM_BUCKETS; i++) {
        b->buckets_internal[i].epoch = 0;
        b->buckets_internal[i].heartbeats = 0;
    }
}

/* Must be called with write lock held */
static void watchdog_buckets_advance_internal(struct watchdog_buckets *buckets,
                                              time_ms_t new_time) {
    if (buckets->last_heartbeat_ms == 0) {
        buckets->last_heartbeat_ms = new_time;
        buckets->buckets_internal[buckets->idx].epoch = buckets->curr_epoch;
        buckets->buckets_internal[buckets->idx].heartbeats = 0;
        return;
    }

    if (unlikely(new_time < buckets->last_heartbeat_ms))
        return;

    size_t old_bucket = time_to_bucket(buckets->last_heartbeat_ms);
    size_t new_bucket = time_to_bucket(new_time);
    if (new_bucket <= old_bucket)
        return;

    size_t elapsed = new_bucket - old_bucket;
    if (elapsed >= WATCHDOG_NUM_BUCKETS) {
        size_t idx_absolute = buckets->idx + elapsed;
        size_t wraps = idx_absolute / WATCHDOG_NUM_BUCKETS;
        buckets->curr_epoch += wraps;
        buckets->idx = idx_absolute % WATCHDOG_NUM_BUCKETS;

        for (size_t i = 0; i < WATCHDOG_NUM_BUCKETS; i++) {
            buckets->buckets_internal[i].heartbeats = 0;
            buckets->buckets_internal[i].epoch = 0;
        }

        buckets->buckets_internal[buckets->idx].epoch = buckets->curr_epoch;
        buckets->buckets_internal[buckets->idx].heartbeats = 0;
    } else {
        for (size_t i = 0; i < elapsed; i++) {
            buckets->idx++;
            if (buckets->idx == WATCHDOG_NUM_BUCKETS) {
                buckets->idx = 0;
                buckets->curr_epoch++;
            }

            size_t idx = buckets->idx;
            buckets->buckets_internal[idx].heartbeats = 0;
            buckets->buckets_internal[idx].epoch = buckets->curr_epoch;
        }
    }

    buckets->last_heartbeat_ms = new_time;
}

/* Only called by percpu owner */
static void watchdog_buckets_inc_heartbeat(struct watchdog_buckets *buckets,
                                           time_ms_t now) {
    seqcount_begin_write(&buckets->seq);
    watchdog_buckets_advance_internal(buckets, now);

    struct watchdog_bucket *bucket = &buckets->buckets_internal[buckets->idx];
    if (bucket->epoch != buckets->curr_epoch) {
        bucket->epoch = buckets->curr_epoch;
        bucket->heartbeats = 0;
    }

    bucket->heartbeats++;
    buckets->last_heartbeat_ms = now;
    seqcount_end_write(&buckets->seq);
}

static void watchdog_do_percpu_heartbeat(time_ms_t now) {
    kassert(PERCPU_READY(watchdog_percpu));

    atomic_fetch_add_explicit(&PERCPU_PTR(watchdog_percpu)->heartbeat_seq, 1,
                              memory_order_relaxed);

    watchdog_buckets_inc_heartbeat(&PERCPU_READ(watchdog_percpu).buckets, now);
}

/* Non blocking snapshot */
static bool watchdog_buckets_snapshot(const struct watchdog_buckets *b,
                                      struct watchdog_bucket_snapshot *out) {
    uint32_t seq;
    int retries = WATCHDOG_MASTER_SEQCOUNT_SPINS;

    /* NOTE: It is highly unlikely, but not impossible,
     * for us to end up in a scenario where a worker somehow gets stuck
     * within the code section under the seqcount write begin/end,
     * which could lead to the master never reading data from its buckets,
     * and never promoting past NORMAL in its criticality
     *
     * TODO: I can only imagine such a scenario happening with a
     * kernel live patch gone wrong, but needless to say, this is
     * a highly unlikely, but theoretically possible state that
     * we may want to track and handle via bail and panic() */
    do {
        seq = seqcount_begin_read_raw(&b->seq);
        if (unlikely((seq & 1) != 0))
            return false;

        out->idx = b->idx;
        out->curr_epoch = b->curr_epoch;
        out->last_heartbeat_ms = b->last_heartbeat_ms;
        for (size_t i = 0; i < WATCHDOG_NUM_BUCKETS; i++)
            out->buckets[i] = b->buckets_internal[i];

    } while (seqcount_read_retry(&b->seq, seq) && --retries > 0);

    return retries > 0;
}

bool watchdog_count_heartbeats_at(const struct watchdog_buckets *buckets,
                                  size_t window_buckets, time_ms_t now,
                                  time_ms_t bucket_interval_ms,
                                  size_t expected_per_bucket,
                                  size_t *out_heartbeats, size_t *out_expected,
                                  fx32_32_t *out_score) {
    struct watchdog_bucket_snapshot snap;
    if (!watchdog_buckets_snapshot(buckets, &snap))
        return false;

    kassert(window_buckets && window_buckets <= WATCHDOG_NUM_BUCKETS);
    kassert(bucket_interval_ms);
    kassert(expected_per_bucket);

    if (snap.last_heartbeat_ms == 0)
        goto not_ready;

    if (unlikely(now < snap.last_heartbeat_ms))
        now = snap.last_heartbeat_ms;

    size_t cursor = snap.curr_epoch * WATCHDOG_NUM_BUCKETS + snap.idx;
    size_t last_bucket = snap.last_heartbeat_ms / bucket_interval_ms;
    size_t now_bucket = now / bucket_interval_ms;
    kassert(last_bucket >= cursor);

    size_t first_bucket = last_bucket - cursor;
    size_t completed = now_bucket - first_bucket;
    if (completed > window_buckets)
        completed = window_buckets;
    if (!completed)
        goto not_ready;

    size_t virtual_cursor = cursor + now_bucket - last_bucket;
    size_t total = 0, total_expect = 0;
    for (size_t i = 1; i <= completed; i++) {
        size_t absolute = virtual_cursor - i;
        size_t idx = absolute % WATCHDOG_NUM_BUCKETS;
        size_t expected_epoch = absolute / WATCHDOG_NUM_BUCKETS;

        if (snap.buckets[idx].epoch == expected_epoch)
            total += snap.buckets[idx].heartbeats;
        total_expect += expected_per_bucket;
    }

    if (!total_expect)
        goto not_ready;

    /* If we're here, out_expect should never be zero, since one tick
     * must fire for something to enter SUSPECT */
    kassert(total_expect);

    if (out_score)
        *out_score = fx_div(fx_from_int(total_expect - total),
                            fx_from_int(total_expect));

    if (out_expected)
        *out_expected = total_expect;

    if (out_heartbeats)
        *out_heartbeats = total;

    return true;

not_ready:
    if (out_heartbeats)
        *out_heartbeats = 0;

    if (out_expected)
        *out_expected = 0;

    if (out_score)
        *out_score = 0;

    return false;
}

static void watchdog_worker_timer_func(struct timer *t) {
    kassert(irq_in_interrupt());

    struct watchdog_percpu *pcpu = PERCPU_PTR(watchdog_percpu);
    enum irql irql = spin_lock_irq_disable(&pcpu->callback_list.lock);

    struct watchdog_callback *cb;
    list_for_each_entry(cb, &pcpu->callback_list.list, list) {
        cb->fn(cb);
    }

    spin_unlock(&pcpu->callback_list.lock, irql);

    time_ms_t now = time_get_ms();
    watchdog_do_percpu_heartbeat(now);

    timer_modify(t, timer_delta_us(NS_TO_US(config.worker_heartbeat_interval)));
}

static void watchdog_start_petting(cpu_id_t cpu) {
    struct watchdog_percpu *pcpu = PERCPU_PTR_FOR_CPU(watchdog_percpu, cpu);
    if (cpu == 0) {
        pcpu->pets_enabled = true;
        pcpu->anti_pets = 0;
        pcpu->pets = 0;
        return; /* We don't touch the seqcount for BSP */
    }

    seqcount_begin_write(&pcpu->pets_seq);
    nmi_send(cpu);
}

static void watchdog_read_pets_for(cpu_id_t cpu, size_t *out_pets,
                                   size_t *out_anti_pets) {
    struct watchdog_percpu *pcpu = PERCPU_PTR_FOR_CPU(watchdog_percpu, cpu);
    *out_pets = atomic_load_explicit(&pcpu->pets, memory_order_relaxed);
    *out_anti_pets =
        atomic_load_explicit(&pcpu->anti_pets, memory_order_relaxed);
}

/* This is the real meat and potatoes of this whole subsystem, and it
 * needs a bit of explanation that is specifically tied to code
 * so the implementation is clear
 *
 * At the high level, the steps are
 *
 * (1) Check on any CRITICAL CPUs -> (2) Check on any SUSPECT CPUs ->
 * (3) Check on NORMAL domains and CPUs -> (4) Panic last, so that
 * we can capture all possible CPUs to panic on in (1) and do
 * any necessary promotions for this tick
 *
 * NOTE: Anything that bothers/inspects another CPU must account for
 * the case where cpu == 0
 */
static void watchdog_cpu_promote(cpu_id_t cpu,
                                 enum watchdog_master_state new_state) {
    kassert(cpu_mask_test(&watchdog_master.cpu_masks[new_state - 1], cpu));
    cpu_mask_clear(&watchdog_master.cpu_masks[new_state - 1], cpu);
    cpu_mask_set(&watchdog_master.cpu_masks[new_state], cpu);
    watchdog_master.cpus[cpu].state = new_state;
}

static void watchdog_cpu_demote(cpu_id_t cpu,
                                enum watchdog_master_state new_state) {
    kassert(cpu_mask_test(&watchdog_master.cpu_masks[new_state + 1], cpu));
    cpu_mask_clear(&watchdog_master.cpu_masks[new_state + 1], cpu);
    cpu_mask_set(&watchdog_master.cpu_masks[new_state], cpu);
    watchdog_master.cpus[cpu].state = new_state;
}

static bool watchdog_test_outstanding(struct watchdog_master_cpu *cpu) {
    return seqcount_read_raw(&cpu->pcpu->response.seqcount) & 1;
}

static time_ms_t watchdog_finished_time(struct watchdog_master_cpu *cpu) {
    return cpu->pcpu->response.finished_ms;
}

static void watchdog_start_test_on(struct watchdog_master_cpu *cpu,
                                   time_ms_t now) {
    /* Cannot be called with seqcount write active */
    struct watchdog_percpu *pcpu = cpu->pcpu;
    struct watchdog_percpu_response *resp = &pcpu->response;

    /* If odd, it's mid-test, cannot start another */
    kassert(!(seqcount_read_raw(&resp->seqcount) & 1));
    seqcount_begin_write(&resp->seqcount);
    cpu->critical_test_start = now;

    /* Fine to not guard against cpu == 0, since the IRQ
     * here is safe as it's not an NMI */
    ipi_send(cpu->id, watchdog_global.critical_test_irq);
}

static time_ms_t watchdog_spin_for_response(struct watchdog_master_cpu *cpu) {
    uint32_t seq;
    int retries = WATCHDOG_MASTER_SEQCOUNT_SPINS;
    struct watchdog_percpu_response *resp = &cpu->pcpu->response;

    do {
        seq = seqcount_begin_read_raw(&resp->seqcount);
        if (seq & 1) {
            cpu_relax();
            continue;
        }

        kassert(resp->finished_ms >= cpu->critical_test_start);
        return resp->finished_ms - cpu->critical_test_start;

    } while ((seqcount_read_retry(&resp->seqcount, seq) || (seq & 1)) &&
             --retries > 0);

    return TIME_MS_MAX;
}

static void watchdog_enter_suspect(cpu_id_t cpu) {
    struct watchdog_master_cpu *mcpu = &watchdog_master.cpus[cpu];

    kassert(mcpu->state == WATCHDOG_STATE_CRITICAL ||
            mcpu->state == WATCHDOG_STATE_NORMAL);

    if (mcpu->state == WATCHDOG_STATE_NORMAL) {
        watchdog_cpu_promote(cpu, WATCHDOG_STATE_SUSPECT);
    } else {
        watchdog_cpu_demote(cpu, WATCHDOG_STATE_SUSPECT);
    }

    /* Demote and reset score, tests have been fine */
    mcpu->lockup_score = config.master_suspect_score;
    mcpu->lockup_ewma.ewma = mcpu->lockup_score;
    mcpu->suspect_start_tick = watchdog_master.tick;
}

static inline void watchdog_record_delta(struct watchdog_master_cpu *mcpu,
                                         time_ms_t delta) {
    mcpu->critical_tests[mcpu->critical_tests_done++] = delta;
}

/* Score pet count against the expected amount over
 * the window */
static inline fx32_32_t watchdog_pets_score(size_t pets) {
    size_t expected = watchdog_global.expected_heartbeats_per_bucket;
    if (!expected)
        return pets > 0 ? FX(0.0) : FX_ONE;

    if (pets >= expected)
        return FX(0.0);

    return fx_div(fx_from_int(expected - pets), fx_from_int(expected));
}

static void watchdog_master_process_critical(time_ms_t now) {
    /* All CPUs in here have been CRITICAL for at *least* one tick,
     * so we never start the petting here, only end + demote
     *
     * Here, we look at each CPU, and for each one, we investigate
     * (1) the pet state, (2) pet status, (3) IPI status
     */

    cpu_id_t i;
    watchdog_cpu_for_each(i, WATCHDOG_STATE_CRITICAL) {
        size_t failures = 0;
        size_t target_tests, pets, anti_pets;
        watchdog_read_pets_for(i, &pets, &anti_pets);

        if (pets > 0) {
            target_tests = WATCHDOG_CRITICAL_IPI_TESTS;
        } else if (!anti_pets) {
            target_tests = (WATCHDOG_CRITICAL_IPI_TESTS * 3) / 4;
        } else {
            target_tests = WATCHDOG_CRITICAL_IPI_TESTS / 2;
        }

        kassert(target_tests);

        struct watchdog_master_cpu *mcpu = &watchdog_master.cpus[i];

        /* TODO: Logging should be safe in NMI context as
         * the log site does not acquire locks when passthrough
         * printing is off */

        /* We have to first check this condition before anything else */
        bool outstanding = false;
        if (watchdog_test_outstanding(mcpu)) {
            outstanding = true;
            if (now - mcpu->critical_test_start >=
                WATCHDOG_CRITICAL_PANIC_THRESHOLD_MS)
                goto promote_to_panic;
        }

        if (mcpu->critical_tests_done >= target_tests)
            goto score;

        if (outstanding)
            continue;

        /* No outstanding tests, record the last one, start another
         *
         * NOTE: the promotion process to CRITICAL will have started the first
         * test in the sequence, we don't handle that here */
        time_ms_t finished = watchdog_finished_time(mcpu);
        kassert(finished >= mcpu->critical_test_start);

        time_ms_t delta = finished - mcpu->critical_test_start;

        /* NOTE: If a test finishes at all, don't panic */
        watchdog_record_delta(mcpu, delta);

        /* Start a new one */
        watchdog_start_test_on(mcpu, now);

        /* Spin a few times to see if we get a response,
         * and record it if we do */
        delta = watchdog_spin_for_response(mcpu);
        if (delta != TIME_MS_MAX)
            watchdog_record_delta(mcpu, delta);

        continue;

    score:
        /* Use the scoring formula:
         *
         * failed/total * TEST_WEIGHT + pet_factor * PET_WEIGHT
         *
         * mapped to [warn_score, 1]
         *
         */

        for (int j = 0; j < WATCHDOG_CRITICAL_IPI_TESTS; j++) {
            if (mcpu->critical_tests[j] > WATCHDOG_CRITICAL_PASS_THRESHOLD_MS)
                failures++;
        }

        fx32_32_t test_part =
            fx_mul(fx_div(fx_from_int(failures),
                          fx_from_int(WATCHDOG_CRITICAL_IPI_TESTS)),
                   WATCHDOG_CRITICAL_TESTS_FACTOR);
        fx32_32_t pet_part =
            fx_mul(watchdog_pets_score(pets), WATCHDOG_CRITICAL_PETS_FACTOR);
        fx32_32_t total = test_part + pet_part;
        kassert(total < FX_ONE);

        if (total == 0) {
            watchdog_enter_suspect(i);
        } else {
            /* map it from [0, 1] -> [warn_score, 1] */
            fx32_32_t mapped =
                fx_map(total, 0, FX_ONE, config.master_critical_score, FX_ONE);

            kassert(IN_RANGE(mapped, config.master_critical_score, FX_ONE));
            mcpu->lockup_score = mapped;

            /* Panic promotion only bothers with a bitmap,
             * everything else stays, since it never demotes from there */
            if (mcpu->lockup_score >= config.master_panic_score) {
            promote_to_panic:
                watchdog_cpu_promote(i, WATCHDOG_STATE_PANIC);
            }
        }
    }
}

static void watchdog_enter_critical(struct watchdog_master_cpu *mcpu,
                                    time_ms_t now) {
    /* The idea here: we'll need to start the tests so
     * the critical processing's invariants hold */
    watchdog_cpu_promote(mcpu->id, WATCHDOG_STATE_CRITICAL);
    memset(&mcpu->critical_tests, 0, sizeof(mcpu->critical_tests));
    mcpu->critical_tests_done = 0;
    mcpu->critical_start_tick = watchdog_master.tick;

    watchdog_start_petting(mcpu->id);

    mcpu->critical_test_start = now;
    watchdog_start_test_on(mcpu, now);
}

static void watchdog_enter_normal(struct watchdog_master_cpu *mcpu) {
    ewma_init(&mcpu->lockup_ewma, WATCHDOG_EWMA_ALPHA);
    watchdog_cpu_demote(mcpu->id, WATCHDOG_STATE_NORMAL);
    mcpu->lockup_score = 0;
}

static void watchdog_master_process_suspect(time_ms_t now) {
    cpu_id_t i;
    watchdog_cpu_for_each(i, WATCHDOG_STATE_SUSPECT) {
        /*
         * SUSPECT CPUs are subject to EWMA monitoring,
         * which can either result in it being good enough,
         * or above warn_score, and if it lingers for
         * long enough in SUSPECT limbo, we emit warnings
         */
        struct watchdog_percpu *pcpu = PERCPU_PTR_FOR_CPU(watchdog_percpu, i);
        struct watchdog_master_cpu *mcpu = &watchdog_master.cpus[i];

        fx32_32_t new;
        if (!watchdog_count_heartbeats_at(
                &pcpu->buckets, WATCHDOG_WINDOW_BUCKETS, now,
                watchdog_global.bucket_interval_ms,
                watchdog_global.expected_heartbeats_per_bucket, NULL, NULL,
                &new))
            continue;

        ewma_update(&mcpu->lockup_ewma, new);

        mcpu->lockup_score = mcpu->lockup_ewma.ewma;
        if (mcpu->lockup_score >= config.master_critical_score) {
            watchdog_enter_critical(mcpu, now);
        } else if (mcpu->lockup_score < config.master_suspect_score) {
            watchdog_enter_normal(mcpu);
        }
    }
}

static void watchdog_master_process_normal(void) {
    cpu_id_t i;
    watchdog_cpu_for_each(i, WATCHDOG_STATE_NORMAL) {
        struct watchdog_percpu *pcpu = PERCPU_PTR_FOR_CPU(watchdog_percpu, i);
        fx32_32_t score;
        if (!watchdog_count_heartbeats_at(
                &pcpu->buckets, WATCHDOG_WINDOW_BUCKETS, time_get_ms(),
                watchdog_global.bucket_interval_ms,
                watchdog_global.expected_heartbeats_per_bucket, NULL, NULL,
                &score))
            continue;

        if (score >= config.master_suspect_score)
            watchdog_enter_suspect(i);
    }
}

static void watchdog_master_process_panic(void) {
    cpu_id_t i;

    /* TODO: Aggregate, print other data, for this first version we
     * just print the first CPU that appears hung */
    cpu_mask_for_each(i, watchdog_master.cpu_masks[WATCHDOG_STATE_PANIC]) {
        watchdog_panic("CPU %zu hard lockup (watchdog missed heartbeats)", i);
    }
}

/* We check here if the heartbeat counter changed, measuring elapsed
 * time in only master ticks. This is because places like time_get_ms()
 * have a seqcount that can get stuck, and this is effectively
 * the "quick check" for the edge case stall */
static void watchdog_master_process_stall(void) {
    if (!watchdog_global.stall_timeout_ticks)
        return;

    /* Give the workers time to arm their timers before judging them. */
    if (watchdog_master.tick < WATCHDOG_STALL_GRACE_TICKS)
        return;

    for (cpu_id_t i = 0; i < global.core_count; i++) {
        struct watchdog_master_cpu *mcpu = &watchdog_master.cpus[i];
        struct watchdog_percpu *pcpu = PERCPU_PTR_FOR_CPU(watchdog_percpu, i);

        uint64_t seen =
            atomic_load_explicit(&pcpu->heartbeat_seq, memory_order_relaxed);

        if (seen != mcpu->last_seen_heartbeat) {
            mcpu->last_seen_heartbeat = seen;
            mcpu->last_progress_tick = watchdog_master.tick;
            continue;
        }

        if (watchdog_master.tick - mcpu->last_progress_tick <
            watchdog_global.stall_timeout_ticks)
            continue;

        watchdog_panic("CPU %zu stalled: no heartbeat for %zu master ticks "
                       "(seq stuck at %llu)",
                       (size_t) i,
                       watchdog_master.tick - mcpu->last_progress_tick,
                       (unsigned long long) seen);
    }
}

static enum irq_result watchdog_master_nmi_handler(void *ctx, irq_t irq,
                                                   struct irq_context *regs) {
    (void) ctx, (void) irq, (void) regs;

    /* Since the other NMI ISRs run well before this one does, we can
     * guarantee that this IS us, since we register last, but we do
     * a CPU ID check to really make sure
     *
     * HACK: We simply use the call site ordering to guarantee list_add_tail
     * happens in the right order, but we will want to probably need to
     * make this more robust, reusable, and less reliant on call ordering */
    if (smp_core_id() != 0)
        return IRQ_NONE; /* Not us */

    watchdog_master.tick++;

    /* Process what could've stalled before anything else */
    watchdog_master_process_stall();

    /* If anything is wedged on the timekeeper seqlock, this
     * prevents the NMI from stalling too, so we only try_ here */
    time_ms_t now;
    if (!time_try_get_ms(&now))
        return IRQ_HANDLED;

    watchdog_master_process_critical(now);
    watchdog_master_process_suspect(now);
    watchdog_master_process_normal();
    watchdog_master_process_panic();

    return IRQ_HANDLED;
}

static enum irq_result watchdog_pet_nmi_handler(void *ctx, irq_t irq,
                                                struct irq_context *regs) {
    (void) ctx, (void) irq, (void) regs;
    struct watchdog_percpu *pcpu = PERCPU_PTR(watchdog_percpu);

    /* Must be set, if it doesn't, something happened */
    if (seqcount_read_raw(&pcpu->pets_seq) & 1) {
        pcpu->pets_enabled = true;
        seqcount_end_write(&pcpu->pets_seq);
        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static enum irq_result watchdog_test_handler(void *ctx, irq_t irq,
                                             struct irq_context *regs) {
    (void) ctx, (void) irq, (void) regs;
    struct watchdog_percpu *pcpu = PERCPU_PTR(watchdog_percpu);

    if (seqcount_read_raw(&pcpu->response.seqcount) & 1) {
        pcpu->response.finished_ms = time_get_ms();
        seqcount_end_write(&pcpu->response.seqcount);
        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

/* We'll want to set up the master and all the workers */
void watchdog_init(void) {
    kassert(PERCPU_READY(watchdog_percpu));

    for (int i = 0; i < WATCHDOG_STATE_MAX; i++) {
        alloc_or_die(
            cpu_mask_init(&watchdog_master.cpu_masks[i], global.core_count));
    }

    alloc_or_die(
        cpu_mask_init(&watchdog_master.scratch_mask, global.core_count));

    cpu_mask_set_all(&watchdog_master.cpu_masks[WATCHDOG_STATE_NORMAL]);
    watchdog_master.cpus =
        kmalloc_or_die(sizeof(struct watchdog_master_cpu) * global.core_count,
                       ALLOC_FLAGS_ZERO);

    watchdog_global.critical_test_irq = irq_alloc_entry();
    irq_register("watchdog_test", watchdog_global.critical_test_irq,
                 watchdog_test_handler, NULL, IRQ_FLAG_NONE);
    irq_set_chip(watchdog_global.critical_test_irq, lapic_get_chip(), NULL);
    irq_register("watchdog_master", IRQ_NMI, watchdog_master_nmi_handler, NULL,
                 IRQ_FLAG_SHARED);

    irq_register("watchdog_pet", IRQ_NMI, watchdog_pet_nmi_handler, NULL,
                 IRQ_FLAG_SHARED);

    for (cpu_id_t i = 0; i < global.core_count; i++) {
        watchdog_master.cpus[i].id = i;
        watchdog_master.cpus[i].state = WATCHDOG_STATE_NORMAL;
        ewma_init(&watchdog_master.cpus[i].lockup_ewma, WATCHDOG_EWMA_ALPHA);
        watchdog_master.cpus[i].lockup_score = FX(0.0);
        watchdog_master.cpus[i].pcpu = PERCPU_PTR_FOR_CPU(watchdog_percpu, i);
        locked_list_init(&PERCPU_PTR_FOR_CPU(watchdog_percpu, i)->callback_list,
                         LOCKED_LIST_INIT_IRQ_DISABLE);
    }
}

void watchdog_start(void) {
    watchdog_global.bucket_interval_ms = NS_TO_MS(config.bucket_interval);
    watchdog_global.expected_heartbeats_per_bucket =
        watchdog_global.bucket_interval_ms /
        NS_TO_MS(config.worker_heartbeat_interval);

    time_ms_t tick_ms = NS_TO_MS(config.master_tick_interval);
    watchdog_global.stall_timeout_ticks =
        (config.master_stall_timeout && tick_ms)
            ? NS_TO_MS(config.master_stall_timeout) / tick_ms
            : 0;

    for (cpu_id_t i = 0; i < global.core_count; i++) {
        watchdog_master.cpus[i].last_seen_heartbeat = 0;
        watchdog_master.cpus[i].last_progress_tick = 0;
    }

    struct watchdog_percpu *pcpu;
    percpu_for_each(watchdog_percpu, pcpu, cpu) {
        timer_modify(&pcpu->timer,
                     timer_delta_us(NS_TO_US(config.master_tick_interval)));
    }

    pit_init();
    pit_wire_periodic_nmi(config.master_tick_interval);
    ioapic_route_isa_nmi(0, /* cpu id */ 0);
}

void watchdog_pet(void) {
    if (PERCPU_READY(watchdog_percpu)) {
        struct watchdog_percpu *this = PERCPU_PTR(watchdog_percpu);
        /* The caller must make sure we cannot be preempted */
        if (this->pets_enabled) {
            kassert(irql_get() >= IRQL_DISPATCH_LEVEL);
            this->pets++;
        }
    }
}

void watchdog_anti_pet(void) {
    if (PERCPU_READY(watchdog_percpu)) {
        struct watchdog_percpu *this = PERCPU_PTR(watchdog_percpu);
        /* The caller must make sure we cannot be preempted */
        if (this->pets_enabled) {
            kassert(irql_get() >= IRQL_DISPATCH_LEVEL);
            this->anti_pets++;
        }
    }
}

void watchdog_callback_add(cpu_id_t id, struct watchdog_callback *cb) {
    struct watchdog_percpu *percpu = PERCPU_PTR_FOR_CPU(watchdog_percpu, id);
    locked_list_add(&percpu->callback_list, &cb->list);
}

void watchdog_callback_remove(cpu_id_t id, struct watchdog_callback *cb) {
    struct watchdog_percpu *percpu = PERCPU_PTR_FOR_CPU(watchdog_percpu, id);
    locked_list_del(&percpu->callback_list, &cb->list);
}
