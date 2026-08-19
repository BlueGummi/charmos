#include <asm.h>
#include <irq/idt.h>
#include <kassert.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <sync/rcu.h>
#include <thread/dpc.h>
#include <thread/workqueue.h>

void scheduler_idle_main(void *nop) {
    (void) nop;
    struct scheduler *sched = global.schedulers[smp_core_id()];

    while (true) {
        disable_interrupts();
        if (scheduler_mark_self_needs_resched(false) ||
            sched->total_thread_count > 0 ||
            sched->completed_rbt.root != NULL) {
            enable_interrupts();
            scheduler_yield();
            continue;
        }

        do_idle_insn();
    }
}
