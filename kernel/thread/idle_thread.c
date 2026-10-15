#include <asm.h>
#include <enforce.h>
#include <irq/idt.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <sync/rcu.h>
#include <thread/dpc.h>
#include <thread/workqueue.h>

void scheduler_idle_main(void *nop) {
    cc_unused(nop);
    struct scheduler *sched = global.schedulers[smp_id_raw()];

    bool woken = false;

    while (true) {
        irq_disable();

        rcu_idle_exit(); /* For potential NMI events */

        if (scheduler_mark_self_needs_resched(false) ||
            sched->total_thread_count > 0 ||
            sched->completed_rbt.root != NULL) {
            irq_enable();
            scheduler_yield();
            continue;
        }

        /* Go yield to try to pick up some work */
        if (woken && atomic_load(&scheduler_data.total_threads) > 0) {
            woken = false;
            irq_enable();
            scheduler_yield();
            continue;
        }

        rcu_idle_enter();
        cpu_idle();
        woken = true;
    }
}
