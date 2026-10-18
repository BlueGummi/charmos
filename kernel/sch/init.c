#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/nofail.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <thread/reaper.h>
#include <thread/workqueue.h>

#include "internal.h"

enum irq_result scheduler_reschedule_isr(struct irq_context *rsp, void *ctx) {
    scheduler_mark_self_needs_resched(true);
    cc_ignore(ctx, rsp);
    return IRQ_HANDLED;
}

static size_t scheduler_thread_get_data(struct rbt_node *n) {
    return thread_from_rq_rbt_node(n)->virtual_runtime_left;
}

static int32_t scheduler_cmp_threads(const struct rbt_node *a,
                                     const struct rbt_node *b) {
    int64_t vrla = thread_from_rq_rbt_node(a)->virtual_runtime_left;
    int64_t vrlb = thread_from_rq_rbt_node(b)->virtual_runtime_left;
    return (vrla > vrlb) - (vrla < vrlb);
}

static void scheduler_tick(struct timer *t) {
    struct scheduler *self = smp_core_scheduler();
    scheduler_mark_self_needs_resched(true);

    if (scheduler_tick_enabled(self)) {
        timer_modify(t, timer_delta_us(MS_TO_US(self->tick_duration_ms)));
    }
}

void scheduler_init(void) {
    scheduler_data.max_concurrent_stealers = global.core_count / 4;

    /* I mean, if we have one core and that core wants
     * to steal work from itself, go ahead? */
    if (scheduler_data.max_concurrent_stealers == 0)
        scheduler_data.max_concurrent_stealers = 1;

    global.schedulers = vnews(global.schedulers, global.core_count);
    if (!global.schedulers)
        panic("Could not allocate scheduler pointer array");

    size_t i;
    for_each_cpu_id(i) {
        struct scheduler *s = nofail_vnew(s, ALLOC_ZERO);

        spinlock_init(&s->lock, LOCK_UNCHKD);
        rbt_init(&s->thread_rbt, scheduler_thread_get_data,
                 scheduler_cmp_threads);
        rbt_init(&s->completed_rbt, scheduler_thread_get_data,
                 scheduler_cmp_threads);
        rbt_init(&s->climb_threads, climb_get_thread_data, climb_cmp_threads);
        atomic_store_rlx(&s->tick_enabled, false);
        s->current_period = 1; /* Start at period 1 to avoid
                                * starting at 0 because
                                * that would lead to threads
                                * being mistakenly identified
                                * as completed */

        cpu_mask_clear_all(&s->deferred_resched_mask);
        s->core_id = i;
        s->tick.func = scheduler_tick;
        s->tick.flags =
            TIMER_FLAG_IRQ | TIMER_FLAG_CPU(s->core_id) | TIMER_FLAG_PINNED;
        struct thread *idle_thread =
            thread_create(("idle_thread_%zu", i), scheduler_idle_main,
                          .flags = THREAD_FLAG_PINNED);
        atomic_store_rlx(&idle_thread->state, THREAD_STATE_IDLE_THREAD);
        s->idle_thread = idle_thread;

        list_head_init(&s->rt_threads);
        list_head_init(&s->urgent_threads);
        list_head_init(&s->bg_threads);

        if (!i) {
            struct thread *t = thread_create("main_thread", k_sch_main,
                                             .flags = THREAD_FLAG_PINNED);
            scheduler_add_thread(s, t);
        }

        global.schedulers[i] = s;
    }
}
