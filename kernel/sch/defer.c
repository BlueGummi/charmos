#include <acpi/hpet.h>
#include <acpi/ioapic.h>
#include <acpi/lapic.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <spin_lock.h>
#include <time/time.h>

static deferred_event_t *defer_queue = NULL;
static struct spinlock defer_lock = {0};
#define HPET_IRQ_VECTOR 0xF2
#define HPET_IRQ_LINE 2

static void hpet_irq_handler(void *ctx, uint8_t irq, void *rsp) {
    (void) irq;
    (void) ctx, (void) rsp;
    uint64_t now = hpet_timestamp_ms();

    bool i = spin_lock(&defer_lock);
    while (defer_queue && defer_queue->timestamp_ms <= now) {
        deferred_event_t *ev = defer_queue;
        defer_queue = ev->next;

        spin_unlock(&defer_lock, i);
        ev->callback(ev->arg);
        kfree(ev);
        i = spin_lock(&defer_lock);
    }

    if (defer_queue) {
        hpet_program_oneshot(defer_queue->timestamp_ms);
    }
    spin_unlock(&defer_lock, i);
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_EOI), 0);
}

void defer_enqueue(defer_func_t func, void *arg, uint64_t delay_ms) {
    uint64_t now = time_get_ms();
    deferred_event_t *ev = kmalloc(sizeof(deferred_event_t));
    ev->timestamp_ms = now + delay_ms;
    ev->callback = func;
    ev->arg = arg;

    bool i = spin_lock(&defer_lock);
    if (!defer_queue || ev->timestamp_ms < defer_queue->timestamp_ms) {
        ev->next = defer_queue;
        defer_queue = ev;
        hpet_program_oneshot(ev->timestamp_ms);
    } else {
        deferred_event_t *curr = defer_queue;
        while (curr->next && curr->next->timestamp_ms < ev->timestamp_ms)
            curr = curr->next;
        ev->next = curr->next;
        curr->next = ev;
    }
    spin_unlock(&defer_lock, i);
}

void defer_init(void) {
    uint8_t vector = idt_alloc_entry_on_core(0);

    idt_set_alloc(vector, 0, true);
    isr_register(vector, hpet_irq_handler, NULL, 0);
    ioapic_route_irq(HPET_IRQ_LINE, vector, 0, false);
}
