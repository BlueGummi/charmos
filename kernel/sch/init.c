#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <thread/reaper.h>
#include <thread/workqueue.h>

static size_t scheduler_thread_get_data(struct rbt_node *n) {
    return thread_from_rq_rbt_node(n)->virtual_runtime_left;
}

static int32_t scheduler_cmp_threads(const struct rbt_node *a,
                                     const struct rbt_node *b) {
    int32_t vrla = thread_from_rq_rbt_node(a)->virtual_runtime_left;
    int32_t vrlb = thread_from_rq_rbt_node(b)->virtual_runtime_left;
    return vrla - vrlb;
}

void scheduler_init(void) {
    scheduler_data.max_concurrent_stealers = global.core_count / 4;

    /* I mean, if we have one core and that core wants
     * to steal work from itself, go ahead? */
    if (scheduler_data.max_concurrent_stealers == 0)
        scheduler_data.max_concurrent_stealers = 1;

    global.schedulers = kmalloc(sizeof(struct scheduler *) * global.core_count);
    if (!global.schedulers)
        panic("Could not allocate scheduler pointer array\n");

    size_t i;
    for_each_cpu_id(i) {
        struct scheduler *s = kzalloc(sizeof(struct scheduler));
        if (!s)
            panic("Could not allocate scheduler %lu\n", i);

        rbt_init(&s->thread_rbt, scheduler_thread_get_data,
                 scheduler_cmp_threads);
        rbt_init(&s->completed_rbt, scheduler_thread_get_data,
                 scheduler_cmp_threads);
        rbt_init(&s->climb_threads, climb_get_thread_data,
                 scheduler_cmp_threads);
        s->tick_enabled = false;
        s->current_period = 1; /* Start at period 1 to avoid
                                * starting at 0 because
                                * that would lead to threads
                                * being mistakenly identified
                                * as completed */

        s->core_id = i;

        struct thread *idle_thread =
            thread_create("idle_thread_%u", scheduler_idle_main, NULL, i);
        idle_thread->pinned = 1;
        idle_thread->state = THREAD_STATE_IDLE_THREAD;
        s->idle_thread = idle_thread;

        INIT_LIST_HEAD(&s->rt_threads);
        INIT_LIST_HEAD(&s->urgent_threads);
        INIT_LIST_HEAD(&s->bg_threads);

        if (!i) {
            struct thread *t = thread_create("main_thread", k_sch_main, NULL);
            t->pinned = 1;
            scheduler_add_thread(s, t, false);
        }

        global.schedulers[i] = s;
    }
}
