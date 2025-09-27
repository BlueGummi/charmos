#include <smp/core.h>

void irq_mark_self_in_interrupt(bool new) {
    smp_core()->in_interrupt = new;
}

bool irq_in_interrupt(void) {
    return smp_core()->in_interrupt;
}

bool irq_in_thread_context(void) {
    return !irq_in_interrupt();
}
