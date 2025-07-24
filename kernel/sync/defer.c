#include <acpi/hpet.h>
#include <acpi/ioapic.h>
#include <acpi/lapic.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sync/spin_lock.h>
#include <time/time.h>

static struct deferred_event *defer_queue = NULL;
static struct spinlock defer_lock = {0};
static uint64_t hpet_next_fire_time = UINT64_MAX;

static void hpet_irq_handler(void *ctx, uint8_t irq, void *rsp) {
    (void) irq, (void) ctx, (void) rsp;
    uint64_t now = hpet_timestamp_ms();

    bool i = spin_lock(&defer_lock);

    while (defer_queue && defer_queue->timestamp_ms <= now) {
        struct deferred_event *ev = defer_queue;
        defer_queue = ev->next;

        spin_unlock(&defer_lock, i);
        ev->callback(ev->arg, ev->arg2);

        defer_free(ev);
        i = spin_lock(&defer_lock);
    }

    if (defer_queue) {
        hpet_next_fire_time = defer_queue->timestamp_ms;
        hpet_program_oneshot(defer_queue->timestamp_ms);
    } else {
        hpet_next_fire_time = UINT64_MAX;
    }

    spin_unlock(&defer_lock, i);

    hpet_clear_interrupt_status();
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_EOI), 0);
}

bool defer_enqueue(dpc_t func, void *arg, void *arg2, uint64_t delay_ms) {
    uint64_t now = hpet_timestamp_ms();
    struct deferred_event *ev = kzalloc(sizeof(struct deferred_event));
    if (!ev)
        return false;

    ev->timestamp_ms = now + delay_ms;
    ev->callback = func;
    ev->arg = arg;
    ev->arg2 = arg2;

    bool i = spin_lock(&defer_lock);
    if (!defer_queue || ev->timestamp_ms < defer_queue->timestamp_ms) {
        ev->next = defer_queue;
        defer_queue = ev;

        if (ev->timestamp_ms < hpet_next_fire_time) {
            hpet_next_fire_time = ev->timestamp_ms;
            hpet_program_oneshot(ev->timestamp_ms);
        }
    } else {
        struct deferred_event *curr = defer_queue;
        while (curr->next && curr->next->timestamp_ms < ev->timestamp_ms)
            curr = curr->next;

        ev->next = curr->next;
        curr->next = ev;
    }

    spin_unlock(&defer_lock, i);
    return true;
}

void defer_init(void) {
    for (uint64_t i = 0; i < hpet_timer_count; i++) {
        uint8_t vector = idt_alloc_entry();

        isr_register(vector, hpet_irq_handler, NULL);
        ioapic_route_irq(i + 3, vector, i, false);
        
        hpet_setup_timer(i, i + 3, false, true);

        k_info("DEFER", K_INFO,
               "Timer %llu routed to IRQ %u (vector %u on core %u)", i,
               i + HPET_IRQ_BASE, vector, i);
    }
}
