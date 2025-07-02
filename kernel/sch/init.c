#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sch/thread.h"

void scheduler_init(uint64_t core_count) {
    c_count = core_count;
    max_concurrent_stealers = c_count / 4;

    /* I mean, if we have one core and that core wants
     * to steal work from itself, go ahead? */
    if (max_concurrent_stealers == 0)
        max_concurrent_stealers = 1;

    local_schs = kmalloc(sizeof(struct scheduler *) * core_count);
    if (!local_schs)
        k_panic("Could not allocate scheduler pointer array\n");

    for (uint64_t i = 0; i < core_count; i++) {
        struct scheduler *s = kzalloc(sizeof(struct scheduler));
        if (!s)
            k_panic("Could not allocate scheduler %lu\n", i);

        s->active = true;
        s->thread_count = 0;
        s->core_id = i;
        s->tick_counter = 0;

        for (int lvl = 0; lvl < MLFQ_LEVELS; lvl++) {
            s->queues[lvl].head = NULL;
            s->queues[lvl].tail = NULL;
        }

        struct thread *t0 = thread_create(k_sch_idle);
        scheduler_add_thread(s, t0, false, false, true);
        t0 = thread_create(k_sch_idle);
        scheduler_add_thread(s, t0, false, false, true);

        if (!i) {
            struct thread *t = thread_create(k_sch_main);
            scheduler_add_thread(s, t, false, false, true);
        }

        local_schs[i] = s;
    }
}
