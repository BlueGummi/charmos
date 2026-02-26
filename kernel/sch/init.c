#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <thread/defer.h>
#include <thread/reaper.h>

static size_t scheduler_thread_get_data(struct rbt_node *n) {
    return thread_from_rq_rbt_node(n)->virtual_runtime_left;
}

void scheduler_init(void) {
    scheduler_data.max_concurrent_stealers = global.core_count / 4;

    /* I mean, if we have one core and that core wants
     * to steal work from itself, go ahead? */
    if (scheduler_data.max_concurrent_stealers == 0)
        scheduler_data.max_concurrent_stealers = 1;

    global.schedulers = kmalloc(sizeof(struct scheduler *) * global.core_count,
                                ALLOC_PARAMS_DEFAULT);
    if (!global.schedulers)
        panic("Could not allocate scheduler pointer array\n");

    size_t i;
    for_each_cpu_id(i) {
        struct scheduler *s =
            kzalloc(sizeof(struct scheduler), ALLOC_PARAMS_DEFAULT);
        if (!s)
            panic("Could not allocate scheduler %lu\n", i);

        rbt_init(&s->thread_rbt, scheduler_thread_get_data);
        rbt_init(&s->completed_rbt, scheduler_thread_get_data);
        rbt_init(&s->climb_threads, climb_get_thread_data);
        s->tick_enabled = false;
        s->current_period = 1; /* Start at period 1 to avoid
                                * starting at 0 because
                                * that would lead to threads
                                * being mistakenly identified
                                * as completed */

        s->core_id = i;

        struct thread *idle_thread =
            thread_create("idle_thread_%u", scheduler_idle_main, NULL, i);
        idle_thread->flags = THREAD_FLAGS_NO_STEAL;
        idle_thread->state = THREAD_STATE_IDLE_THREAD;
        s->idle_thread = idle_thread;

        INIT_LIST_HEAD(&s->rt_threads);
        INIT_LIST_HEAD(&s->urgent_threads);
        INIT_LIST_HEAD(&s->bg_threads);

        if (!i) {
            struct thread *t = thread_create("main_thread", k_sch_main, NULL);
            t->flags = THREAD_FLAGS_NO_STEAL;
            scheduler_add_thread(s, t, false);
        }

        global.schedulers[i] = s;
    }
}
