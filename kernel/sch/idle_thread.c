#include <asm.h>
#include <block/sched.h>
#include <int/idt.h>
#include <kassert.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <types/rcu.h>

static inline void reset_idle_thread_stage(struct idle_thread_data *idle) {
    atomic_store(&idle->state, IDLE_THREAD_WORK_STEAL);
    scheduler_yield();
}

static inline void progress_idle_thread_stage(struct idle_thread_data *idle) {
    /* We should not be progressing if we are at the last stage */
    kassert(idle->state < IDLE_THREAD_SLEEP);
    atomic_fetch_add(&idle->state, 1);
}

static void do_idle_work_steal(struct idle_thread_data *idle,
                               struct scheduler *sched) {
    struct thread *stolen = scheduler_try_do_steal(sched);

    if (stolen) {
        /* We got work, so let's reset the stage and yield */
        reset_idle_thread_stage(idle);
    } else {

        /* No work stolen, progress. */
        progress_idle_thread_stage(idle);
    }
}

/* TODO: Make this use C-states once we get that worked out */
static void do_idle_deep_sleep(struct idle_thread_data *data) {
    (void) data;
}

static void do_idle_loop(struct idle_thread_data *idle,
                         struct scheduler *sched) {
    switch (idle->state) {
    case IDLE_THREAD_WORK_STEAL: do_idle_work_steal(idle, sched); break;
    case IDLE_THREAD_SLEEP: do_idle_deep_sleep(idle); break;
    }
}

void scheduler_idle_main(void) {
    struct idle_thread_data *idle = smp_core_idle_thread();

    atomic_store(&idle->woken_from_timer, false);
    atomic_store(&idle->last_entry_ms, time_get_ms());
    atomic_store(&idle->state, IDLE_THREAD_WORK_STEAL);

    while (true) {
        mark_self_idle(true);
        rcu_mark_quiescent();
        enable_interrupts();
        wait_for_interrupt();
    }

    struct scheduler *sched = get_this_core_sched();
    while (true) {
        do_idle_loop(idle, sched);
    }
}
