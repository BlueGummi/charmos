#include <asm.h>
#include <int/idt.h>
#include <kassert.h>
#include <thread/defer.h>
#include <thread/dpc.h>
#include <sch/sched.h>
#include <sync/rcu.h>

void scheduler_idle_main(void *nop) {
    (void) nop;

    while (true) {
        enable_interrupts();
        scheduler_resched_if_needed();
        wait_for_interrupt();
    }
}
