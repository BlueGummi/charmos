#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void scheduler_init(void) {
    scheduler_data.max_concurrent_stealers = global.core_count / 4;

    /* I mean, if we have one core and that core wants
     * to steal work from itself, go ahead? */
    if (scheduler_data.max_concurrent_stealers == 0)
        scheduler_data.max_concurrent_stealers = 1;

    global.schedulers = kmalloc(sizeof(struct scheduler *) * global.core_count);
    if (!global.schedulers)
        k_panic("Could not allocate scheduler pointer array\n");

    for (uint64_t i = 0; i < global.core_count; i++) {
        struct scheduler *s = kzalloc(sizeof(struct scheduler));
        if (!s)
            k_panic("Could not allocate scheduler %lu\n", i);

        s->thread_rbt.root = NULL;
        s->completed_rbt.root = NULL;
        s->tick_enabled = false;
        s->current_period = 1; /* Start at period 1 to avoid
                                * starting at 0 because
                                * that would lead to threads
                                * being mistakenly identified
                                * as completed */

        s->thread_count = 0;
        s->core_id = i;

        struct thread *idle_thread = thread_create(scheduler_idle_main);
        idle_thread->flags = THREAD_FLAGS_NO_STEAL;
        idle_thread->state = THREAD_STATE_IDLE_THREAD;
        s->idle_thread = idle_thread;

        if (!i) {
            struct thread *t = thread_create(k_sch_main);
            t->flags = THREAD_FLAGS_NO_STEAL;
            scheduler_add_thread(s, t, false);
        }

        global.schedulers[i] = s;
    }
    reaper_init();
    workqueues_permanent_init();
}
