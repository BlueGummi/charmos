#include <acpi/hpet.h>
#include <acpi/ioapic.h>
#include <acpi/lapic.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sync/spinlock.h>

static struct deferred_event **defer_queues = NULL;
static struct spinlock *defer_locks = NULL;
static uint64_t *hpet_next_fire_times = NULL;

static inline uint64_t this_timer(void) {
    return HPET_CURRENT;
}

static inline struct spinlock *this_lock(void) {
    return &defer_locks[this_timer()];
}

static inline struct deferred_event *this_defer_queue(void) {
    return defer_queues[this_timer()];
}

static void hpet_irq_handler(void *ctx, uint8_t irq, void *rsp) {
    (void) irq, (void) ctx, (void) rsp;

    struct spinlock *lock = this_lock();
    enum irql irql = spin_lock_irq_disable(lock);

    struct deferred_event *defer_queue = this_defer_queue();

    uint64_t timer = this_timer();

    uint64_t now = hpet_timestamp_ms();
    while (defer_queue && defer_queue->timestamp_ms <= now) {
        struct deferred_event *ev = defer_queue;
        defer_queue = ev->next;
        defer_queues[timer] = defer_queue;

        spin_unlock(lock, irql);

        if (ev->callback)
            ev->callback(ev->arg, ev->arg2);

        kfree(ev);
        irql = spin_lock_irq_disable(lock);
    }

    if (defer_queue) {
        hpet_next_fire_times[timer] = defer_queue->timestamp_ms;
        hpet_program_oneshot(defer_queue->timestamp_ms);
    } else {
        hpet_next_fire_times[timer] = UINT64_MAX;
    }

    spin_unlock(lock, irql);

    hpet_clear_interrupt_status();
    lapic_write(LAPIC_REG_EOI, 0);
}

bool defer_enqueue(dpc_t func, struct work_args args, uint64_t delay_ms) {
    struct deferred_event *ev = kzalloc(sizeof(struct deferred_event));
    if (!ev)
        return false;

    uint64_t now = hpet_timestamp_ms();
    ev->timestamp_ms = now + delay_ms;
    ev->callback = func;
    ev->arg = args.arg1;
    ev->arg2 = args.arg2;

    struct spinlock *lock = this_lock();
    enum irql irql = spin_lock_irq_disable(lock);

    uint64_t t = this_timer();

    if (!defer_queues[t] || ev->timestamp_ms < defer_queues[t]->timestamp_ms) {
        ev->next = defer_queues[t];
        defer_queues[t] = ev;

        if (ev->timestamp_ms < hpet_next_fire_times[t]) {
            hpet_next_fire_times[t] = ev->timestamp_ms;
            hpet_program_oneshot(ev->timestamp_ms);
        }
    } else {
        struct deferred_event *curr = defer_queues[t];
        while (curr->next && curr->next->timestamp_ms < ev->timestamp_ms)
            curr = curr->next;

        ev->next = curr->next;
        curr->next = ev;
    }

    spin_unlock(lock, irql);
    return true;
}

void defer_init(void) {
    defer_queues = kzalloc(sizeof(struct deferred_event *) * hpet_timer_count);
    defer_locks = kzalloc(sizeof(struct spinlock) * hpet_timer_count);
    hpet_next_fire_times = kzalloc(sizeof(uint64_t) * hpet_timer_count);

    if (!defer_queues || !defer_locks || !hpet_next_fire_times)
        k_panic("Defer queue allocation failed!\n");

    for (uint64_t i = 0; i < hpet_timer_count; i++) {
        hpet_next_fire_times[i] = UINT64_MAX;

        uint8_t vector = idt_alloc_entry();

        isr_register(vector, hpet_irq_handler, NULL);
        ioapic_route_irq(i + 3, vector, i, false);

        hpet_setup_timer(i, i + 3, false, true);

        k_info("DEFER", K_INFO,
               "Timer %llu routed to IRQ %u (vector %u on core %u)", i,
               i + HPET_IRQ_BASE, vector, i);
    }
}
