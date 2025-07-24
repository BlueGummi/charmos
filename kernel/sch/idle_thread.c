#include <asm.h>
#include <assert.h>
#include <int/idt.h>
#include <sch/defer.h>
#include <sch/sched.h>

static void timer_wakeup(void *data, void *c) {
    uint64_t target_core = (uint64_t) c;

    struct idle_thread_data *idle = data;
    idle->woken_from_timer = true;

    /* Send the NO-OP interrupt to bring
     * the core out of the halt so it
     * can progress to the next stage */
    lapic_send_ipi(target_core, IRQ_NOP);
}

static inline void setup_idle_timer(struct idle_thread_data *idle, time_t ms) {
    defer_enqueue(timer_wakeup, idle, (void *) get_this_core_id(), ms);
}

/* When we 'go back' in idle thread stages,
 * we must reset to the halt-loop.
 * Going back one or two stages at a time
 * is not beneficial, because if those stages
 * are not the halt-loop, they unnecessarily
 * burn CPU to perform computation. We are already past
 * those stages and can thus likely take a guess
 * and say that those stages will not succeed
 * in bringing us out of the idle thread, since they
 * failed to bring us out of the idle thread last time*/
static inline void reset_idle_thread_stage(struct idle_thread_data *idle) {
    idle->state = IDLE_THREAD_FAST_HLT;
}

static inline void progress_idle_thread_stage(struct idle_thread_data *idle) {
    /* We should not be progressing if we are at the last stage */
    assert(idle->state < IDLE_THREAD_DEEP_SLEEP);
    idle->state++;
}

static void do_idle_fast_hlt(struct idle_thread_data *idle) {
    setup_idle_timer(idle, IDLE_THREAD_CHECK_MS);

    enable_interrupts();
    wait_for_interrupt();

    /* If the very next interrupt is from the timer
     * that we set, then we progress to the next
     * idle thread stage. Otherwise, something else
     * (device driver, scheduler, etc.) has woken us */
    if (idle->woken_from_timer == true) {

        /* Progress to next stage */
        progress_idle_thread_stage(idle);
    } else {

        /* Go back to first stage, but don't yield because
         * this doesn't necessarily mean that the scheduler
         * has found work for our core, just that
         * some other activity is occurring. */
        reset_idle_thread_stage(idle);
    }
}

static void do_idle_work_steal(struct idle_thread_data *idle,
                               struct scheduler *sched) {

    /* There is no need to lock `sched` here, because
     * the lock is only useful if preemption is happening
     * or if other cores are stealing from us. If we are in the idle
     * thread, neither of these things are happening */
    struct thread *stolen = scheduler_try_do_steal(sched);
    if (stolen) {
        /* We got work, so let's reset the stage and yield */
        reset_idle_thread_stage(idle);
        scheduler_yield();
    } else {

        /* No work stolen, progress. */
        progress_idle_thread_stage(idle);
    }
}

static void do_idle_event_scan(struct idle_thread_data *idle) {
    /* I currently have not implemented the logic
     * to do this full event scan, and it is likely not
     * very beneficial since this is high-cost, low-return */

    progress_idle_thread_stage(idle);
}

static void do_idle_loop(struct idle_thread_data *idle,
                         struct scheduler *sched) {
    switch (idle->state) {
    case IDLE_THREAD_FAST_HLT: do_idle_fast_hlt(idle); break;
    case IDLE_THREAD_WORK_STEAL: do_idle_work_steal(idle, sched); break;
    case IDLE_THREAD_EVENT_SCAN: do_idle_event_scan(idle); break;
   // case IDLE_THREAD_DEEP_SLEEP: do_idle_deep_sleep(idle); break;
    }
}

void scheduler_idle_main(void) {
    while(1)asm("sti;hlt");

    struct idle_thread_data *idle = get_this_core_idle_thread();
    idle->woken_from_timer = false;
    idle->last_entry_ms = time_get_ms();
    idle->state = IDLE_THREAD_FAST_HLT;

    struct scheduler *sched = get_this_core_sched();
    scheduler_disable_timeslice();
    enable_interrupts();

    while (true) {
        do_idle_loop(idle, sched);
    }
}
