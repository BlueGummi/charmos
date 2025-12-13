#include <acpi/lapic.h>
#include <irq/irq.h>
#include <sch/sched.h>

enum irq_result scheduler_timer_isr(void *ctx, uint8_t vector,
                                    struct irq_context *rsp) {
    scheduler_mark_self_needs_resched(true);

    lapic_write(LAPIC_REG_EOI, 0);
    (void) ctx, (void) vector, (void) rsp;
    return IRQ_HANDLED;
}
