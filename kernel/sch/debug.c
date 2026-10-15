#include <console/panic.h>
#include <global.h>
#include <log.h>
#include <sch/sched.h>
#include <watchdog.h>

#include "internal.h"

#ifdef DEBUG_SCHED

#define SCHED_DEBUG_STRAND_MS 5000

/* Actually count */
static size_t sched_debug_count_tree(struct scheduler *sched, struct rbt *tree,
                                     const char *which) {
    size_t n = 0;
    struct rbt_node *prev = NULL;
    struct rbt_node *node;

    rbt_for_each(node, tree) {
        enforce(!prev || tree->compare(prev, node) <= 0,
                "cpu %zu %s out of key order", (size_t) sched->core_id, which);
        prev = node;
        n++;
    }

    return n;
}

static size_t sched_debug_count_list(struct list_head *q) {
    size_t n = 0;
    struct list_node *pos;

    list_for_each(pos, q) {
        n++;
    }

    return n;
}

void scheduler_debug_validate(struct scheduler *sched) {
    size_t linked[THREAD_PRIO_CLASS_COUNT] = {0};

    linked[THREAD_PRIO_CLASS_TIMESHARE] =
        sched_debug_count_tree(sched, &sched->thread_rbt, "thread_rbt") +
        sched_debug_count_tree(sched, &sched->completed_rbt, "completed_rbt");

    linked[THREAD_PRIO_CLASS_URGENT] =
        sched_debug_count_list(&sched->urgent_threads);
    linked[THREAD_PRIO_CLASS_REALTIME] =
        sched_debug_count_list(&sched->rt_threads);
    linked[THREAD_PRIO_CLASS_BACKGROUND] =
        sched_debug_count_list(&sched->bg_threads);

    uint8_t bitmap = atomic_load_rlx(&sched->queue_bitmap);
    size_t total = 0;

    for (size_t prio = 0; prio < THREAD_PRIO_CLASS_COUNT; prio++) {
        enforce(linked[prio] == sched->thread_count[prio],
                "cpu %zu class %zu: %zu linked, %zu counted",
                (size_t) sched->core_id, prio, linked[prio],
                sched->thread_count[prio]);

        bool bit = bitmap & (1u << prio);
        enforce(bit == (linked[prio] != 0),
                "cpu %zu class %zu: bitmap says %d with %zu linked",
                (size_t) sched->core_id, prio, (int) bit, linked[prio]);

        total += linked[prio];
    }

    enforce(total == sched->total_thread_count,
            "cpu %zu: %zu linked, total_thread_count %zu",
            (size_t) sched->core_id, total, sched->total_thread_count);
}

static void sched_debug_dump(cpu_id_t cpu, uint64_t now_ms) {
    struct scheduler *s = global.schedulers[cpu];
    struct core *c = global.cores[cpu];
    struct thread *curr = s->current;

    log_msg(LOG_ERROR,
            "rq %zu: queued=%zu [bg %zu ts %zu rt %zu urg %zu] bitmap=0x%x",
            (size_t) cpu, s->total_thread_count, s->thread_count[0],
            s->thread_count[1], s->thread_count[2], s->thread_count[3],
            (unsigned) atomic_load_rlx(&s->queue_bitmap));

    log_msg(LOG_ERROR,
            "rq %zu: idle=%d needs_resched=%d tick=%d progress %lu ms ago",
            (size_t) cpu, (int) atomic_load_rlx(&c->idle),
            (int) atomic_load_rlx(&c->needs_resched),
            (int) atomic_load_rlx(&s->tick_enabled),
            (unsigned long) (now_ms - atomic_load_rlx(&s->debug_progress_ms)));

    log_msg(LOG_ERROR, "rq %zu: current '%s' state=%d", (size_t) cpu,
            curr ? curr->name : "?", curr ? (int) thread_get_state(curr) : -1);
}

static atomic_bool sched_debug_tripped = false;

static void sched_debug_strand_check(struct watchdog_callback *cb) {
    cc_unused(cb);

    if (atomic_load_rlx(&sched_debug_tripped))
        return;

    uint64_t now_ms = (uint64_t) time_get_ms();
    size_t i;

    for_each_cpu_id(i) {
        struct scheduler *s = global.schedulers[i];

        if (s->total_thread_count == 0)
            continue;

        uint64_t progress = atomic_load_rlx(&s->debug_progress_ms);
        if (!progress || progress >= now_ms ||
            now_ms - progress < SCHED_DEBUG_STRAND_MS)
            continue;

        /* RT threads run with tick off and can hold the core */
        struct thread *curr = s->current;
        if (curr && thread_is_rt(curr))
            continue;

        if (atomic_exchange(&sched_debug_tripped, true))
            return;

        size_t j;
        for_each_cpu_id(j) {
            sched_debug_dump(j, now_ms);
        }

        panic("rq %zu stranded: %zu queued, nothing picked for %lu ms", i,
              s->total_thread_count, (unsigned long) (now_ms - progress));
    }
}

static struct watchdog_callback sched_debug_callbacks[CPU_MASK_BITS];

void scheduler_debug_start(void) {
    uint64_t now_ms = (uint64_t) time_get_ms();
    size_t i;

    for_each_cpu_id(i) {
        atomic_store_rlx(&global.schedulers[i]->debug_progress_ms, now_ms);
        sched_debug_callbacks[i].fn = sched_debug_strand_check;
        watchdog_callback_add((cpu_id_t) i, &sched_debug_callbacks[i]);
    }
}

#endif /* DEBUG_SCHED */
