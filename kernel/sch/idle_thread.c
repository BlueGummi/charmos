#include <asm.h>
#include <int/idt.h>
#include <kassert.h>
#include <sch/defer.h>
#include <sch/dpc.h>
#include <sch/sched.h>
#include <sync/rcu.h>

void scheduler_idle_main(void) {

    while (true) {
        enable_interrupts();
        rcu_mark_quiescent();
        scheduler_resched_if_needed();
        wait_for_interrupt();
    }
}
