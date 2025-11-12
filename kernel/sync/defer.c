#include <acpi/hpet.h>
#include <acpi/ioapic.h>
#include <acpi/lapic.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>

struct deferred_event_queue {
    struct deferred_event *head;
    size_t timer;
    struct spinlock lock;
    size_t next_fire_time;
    struct semaphore semaphore;
    struct work work;
};

static struct deferred_event_queue *defer_queues = NULL;
static struct workqueue *defer_workqueue;

static inline uint64_t this_timer(void) {
    return HPET_CURRENT;
}

static inline struct deferred_event_queue *this_defer_queue(void) {
    return &defer_queues[this_timer()];
}

static void hpet_work(void *a, void *b) {
    while (true) {
        time_t now = hpet_timestamp_ms();
        struct deferred_event_queue *defer_queue = a;
        struct deferred_event *head = defer_queue->head;

        enum irql irql = spin_lock_irq_disable(&defer_queue->lock);
        while (head && head->timestamp_ms <= now) {
            struct deferred_event *ev = head;
            head = ev->next;
            defer_queue->head = head;

            spin_unlock(&defer_queue->lock, irql);

            if (ev->callback)
                ev->callback(ev->args.arg1, ev->args.arg2);

            kfree(ev);
            irql = spin_lock_irq_disable(&defer_queue->lock);
        }

        if (defer_queue->head) {
            defer_queue->next_fire_time = defer_queue->head->timestamp_ms;
            hpet_program_oneshot(defer_queue->head->timestamp_ms);
        } else {
            defer_queue->next_fire_time = UINT64_MAX;
        }

        spin_unlock(&defer_queue->lock, irql);
        semaphore_wait(&defer_queue->semaphore);
    }
}

static void hpet_irq_handler(void *ctx, uint8_t irq, void *rsp) {
    (void) irq, (void) ctx, (void) rsp;

    struct deferred_event_queue *defer_queue = this_defer_queue();

    semaphore_post(&defer_queue->semaphore);

    hpet_clear_interrupt_status();
    lapic_write(LAPIC_REG_EOI, 0);
}

bool defer_enqueue(work_function func, struct work_args args,
                   uint64_t delay_ms) {
    struct deferred_event *ev = kzalloc(sizeof(struct deferred_event));
    if (!ev)
        return false;

    uint64_t now = hpet_timestamp_ms();
    ev->timestamp_ms = now + delay_ms;
    ev->callback = func;
    ev->args = args;

    struct deferred_event_queue *queue = this_defer_queue();
    enum irql irql = spin_lock(&queue->lock);

    if (!queue->head || ev->timestamp_ms < queue->head->timestamp_ms) {

        ev->next = queue->head;
        queue->head = ev;

        if (ev->timestamp_ms < queue->next_fire_time) {
            queue->next_fire_time = ev->timestamp_ms;
            hpet_program_oneshot(ev->timestamp_ms);
        }
    } else {
        struct deferred_event *curr = queue->head;
        while (curr->next && curr->next->timestamp_ms < ev->timestamp_ms)
            curr = curr->next;

        ev->next = curr->next;
        curr->next = ev;
    }

    spin_unlock(&queue->lock, irql);
    return true;
}

void defer_init(void) {
    defer_queues =
        kzalloc(sizeof(struct deferred_event_queue) * hpet_timer_count);
    if (!defer_queues)
        k_panic("Defer queue allocation failed!\n");

    struct workqueue_attributes attrs = {
        .capacity = WORKQUEUE_DEFAULT_CAPACITY,
        .flags = WORKQUEUE_FLAG_ON_DEMAND | WORKQUEUE_FLAG_MIGRATABLE_WORKERS,
        .max_workers = hpet_timer_count,
        .min_workers = hpet_timer_count,
        .inactive_check_period =
            {
                .min = WORKQUEUE_DEFAULT_MIN_INACTIVE_CHECK_PERIOD,
                .max = WORKQUEUE_DEFAULT_MAX_INACTIVE_CHECK_PERIOD,
            },
        .spawn_delay = WORKQUEUE_DEFAULT_SPAWN_DELAY,
    };
    defer_workqueue = workqueue_create(&attrs, /* fmt = */ NULL);

    for (uint64_t i = 0; i < hpet_timer_count; i++) {
        work_init(&defer_queues[i].work, hpet_work,
                  WORK_ARGS(&defer_queues[i], NULL));

        defer_queues[i].next_fire_time = UINT64_MAX;
        defer_queues[i].timer = i;
        workqueue_enqueue(defer_workqueue, &defer_queues[i].work);
        semaphore_init(&defer_queues[i].semaphore, 0, SEMAPHORE_INIT_IRQ_DISABLE);
        spinlock_init(&defer_queues[i].lock);

        uint8_t vector = irq_alloc_entry();

        irq_register(vector, hpet_irq_handler, NULL);
        ioapic_route_irq(i + 3, vector, i, false);

        hpet_setup_timer(i, i + 3, false, true);

        k_info("DEFER", K_INFO,
               "Timer %llu routed to IRQ %u (vector %u on core %u)", i,
               i + HPET_IRQ_BASE, vector, i);
    }
}
