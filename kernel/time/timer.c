#include <cmdline.h>
#include <math/clamp.h>
#include <math/min_max.h>
#include <mem/alloc.h>
#include <smp/percpu.h>
#include <time/names.h>
#include <time/timer.h>

#include "internal.h"

CMDLINE_ENTRY_DECLARE(timer,
                      .flags = CMDLINE_ENTRY_SYMBOLIC |
                               CMDLINE_ENTRY_DOCUMENTED,
                      .desc = "Timer subsystem cmdline entries");

CMDLINE_ENTRY_DECLARE(timer_evdev, .name = "clock_evdev",
                      .parent = CMDLINE_ENTRY(timer),
                      .flags = CMDLINE_ENTRY_DOCUMENTED,
                      .desc = "Timer subsystem clock event device",
                      .arg = "<string>", .default_val = CLOCK_NAME_LAPIC,
                      .value = &clock_global.timer_clock_evdev);

static void timer_dpc(void *ctx);
static void timer_percpu_ctor(struct timer_percpu *p, cpu_id_t cpu) {
    for (int i = 0; i < TIMER_BASE_MAX; i++) {
        struct timer_base *pcpu = &p->bases[i];
        pcpu->percpu = p;
        pcpu->cpu = cpu;
        pcpu->type = i;
        spinlock_init(&pcpu->lock);
    }

    dpc_init(&p->timer_dpc, timer_dpc, p);
}

PERCPU_DECLARE(timer_percpu, struct timer_percpu, timer_percpu_ctor);

static uint32_t wheel_index_for_level(time_us_t expiration, uint32_t level,
                                      time_us_t *bucket_expiration) {
    expiration = (expiration >> TIMER_LEVEL_SHIFT(level)) + 1;
    *bucket_expiration = expiration << TIMER_LEVEL_SHIFT(level);
    return TIMER_LEVEL_OFFSET(level) + (expiration & TIMER_LEVEL_MASK);
}

static uint32_t wheel_index_for(time_us_t expiration, time_us_t now,
                                time_us_t *bucket_expiration) {
    time_us_t delta = expiration - now;

    /* Passed */
    if (expiration < now) {
        *bucket_expiration = now;
        return now & TIMER_LEVEL_MASK;
    }

    if (delta < TIMER_LEVEL_START(1)) {
        return wheel_index_for_level(expiration, 0, bucket_expiration);
    } else if (delta < TIMER_LEVEL_START(2)) {
        return wheel_index_for_level(expiration, 1, bucket_expiration);
    } else if (delta < TIMER_LEVEL_START(3)) {
        return wheel_index_for_level(expiration, 2, bucket_expiration);
    } else if (delta < TIMER_LEVEL_START(4)) {
        return wheel_index_for_level(expiration, 3, bucket_expiration);
    } else if (delta < TIMER_LEVEL_START(5)) {
        return wheel_index_for_level(expiration, 4, bucket_expiration);
    } else if (delta < TIMER_LEVEL_START(6)) {
        return wheel_index_for_level(expiration, 5, bucket_expiration);
    } else if (delta < TIMER_LEVEL_START(7)) {
        return wheel_index_for_level(expiration, 6, bucket_expiration);
    } else if (delta < TIMER_LEVEL_START(8)) {
        return wheel_index_for_level(expiration, 7, bucket_expiration);
    } else if (delta < TIMER_LEVEL_START(9)) {
        return wheel_index_for_level(expiration, 8, bucket_expiration);
    } else if (delta < TIMER_LEVEL_START(10)) {
        return wheel_index_for_level(expiration, 9, bucket_expiration);
    } else {
        if (delta >= TIMER_WHEEL_TIMEOUT_CUTOFF)
            expiration = now + TIMER_WHEEL_TIMEOUT_MAX;

        return wheel_index_for_level(expiration, TIMER_LEVELS - 1,
                                     bucket_expiration);
    }
}

static void timer_enqueue_internal(struct timer_base *base, struct timer *timer,
                                   uint32_t idx, time_us_t bucket_expiration) {
    hlist_add_head(&timer->hlist_node, base->buckets + idx);
    bitmap_set(base->pending_map, idx);
    timer_bucket_set(timer, idx);

    if (bucket_expiration < base->next_expiration_us) {
        base->next_expiration_us = bucket_expiration;
        base->pending = true;
        base->next_expiration_recalc = false;

        timer_base_reprogram_hardware(base->cpu);
    }
}

static void timer_add_internal(struct timer_base *base, struct timer *timer) {
    time_us_t bucket_expiration;
    uint32_t idx =
        wheel_index_for(timer->expiration_us, base->clock, &bucket_expiration);
    timer_enqueue_internal(base, timer, idx, bucket_expiration);
}

static inline struct timer_base *timer_base_for_cpu(enum timer_flags flags,
                                                    cpu_id_t cpu) {
    enum timer_base_type type =
        flags & TIMER_FLAG_PINNED ? TIMER_BASE_LOCAL : TIMER_BASE_GLOBAL;

    if (flags & TIMER_FLAG_DEFERRABLE)
        type = TIMER_BASE_DEFERRED;

    return &(PERCPU_PTR_FOR_CPU(timer_percpu, cpu)->bases[type]);
}

static inline struct timer_base *timer_base_for_flags(enum timer_flags flags) {
    return timer_base_for_cpu(flags, flags & TIMER_FLAG_CPU_MASK);
}

static inline void timer_list_del(struct timer *timer) {
    struct hlist_node *node = &timer->hlist_node;
    hlist_del(node);
}

static inline void timer_fn_call(struct timer *timer) {
    timer->func(timer);
}

static inline void timer_sync_wait_spin() {
    for (int i = 0; i < TIMER_SYNC_SPIN_TIMES; i++)
        cpu_relax();
}

static void timer_dpc(void *ctx) {
    struct timer_percpu *pcpu = ctx;

    enum irql irql = spin_lock_irq_disable(&pcpu->lock);

    while (!hlist_empty(&pcpu->dpc_timers)) {
        struct timer *timer =
            hlist_entry(pcpu->dpc_timers.first, struct timer, hlist_node);
        struct timer_base *base = timer_base_for_flags(timer->flags);

        enum irql birql = spin_lock_irq_disable(&base->lock);

        base->running = timer;

        spin_unlock(&base->lock, birql);

        timer_list_del(timer);
        spin_unlock(&pcpu->lock, irql);

        kassert(!(timer->flags & TIMER_FLAG_IRQ));
        timer_fn_call(timer);

        irql = spin_lock_irq_disable(&pcpu->lock);
    }

    spin_unlock(&pcpu->lock, irql);
}

/* Since we have two types of timers, DPC and IRQ ones, we have to construct
 * the DPC list in here, and make sure we don't invert anything */
static void timer_expire_bucket(struct timer_base *base,
                                struct hlist_head *head, enum irql *lirql) {
    while (!hlist_empty(head)) {
        struct timer *timer =
            hlist_entry(head->first, struct timer, hlist_node);
        timer_list_del(timer);

        kassert(timer->func);

        if (timer->flags & TIMER_FLAG_IRQ) {
            base->running = timer;
            spin_unlock(&base->lock, *lirql);
            timer_fn_call(timer);
            *lirql = spin_lock_irq_disable(&base->lock);
            base->running = NULL;
        } else {
            enum irql pirql = spin_lock_irq_disable(&base->percpu->lock);

            hlist_add_head(&timer->hlist_node, &base->percpu->dpc_timers);
            dpc_enqueue_local(&base->percpu->timer_dpc, DPC_NONE);

            spin_unlock(&base->percpu->lock, pirql);
        }
    }
}

static size_t timer_collect_expired(struct timer_base *base,
                                    struct hlist_head *heads) {
    time_us_t clock = base->clock = base->next_expiration_us;
    size_t levels = 0;

    for (int i = 0; i < TIMER_LEVELS; i++) {
        uint32_t idx = (clock & TIMER_LEVEL_MASK) + i * TIMER_LEVEL_SIZE;

        if (bitmap_test_and_clear(base->pending_map, idx)) {
            struct hlist_head *tmp = base->buckets + idx;
            hlist_move_list(tmp, heads++);
            levels++;
        }

        if (clock & TIMER_CLOCK_MASK)
            break;

        clock >>= TIMER_CLOCK_SHIFT;
    }

    return levels;
}

static int32_t timer_next_pending(struct timer_base *base, uint32_t offset,
                                  time_us_t clock) {
    uint32_t end = offset + TIMER_LEVEL_SIZE;
    uint32_t start = offset + clock;

    uint32_t pos = bitmap_find_next_bit(base->pending_map, end, start);
    if (pos < end)
        return pos - start;

    pos = bitmap_find_next_bit(base->pending_map, start, offset);
    return pos < start ? pos + TIMER_LEVEL_SIZE - start : -1;
}

static void timer_recalc_next_expiration(struct timer_base *base) {
    SPINLOCK_ASSERT_HELD(&base->lock);
    time_us_t next = TIME_US_MAX;
    time_us_t clock = base->clock;

    for (int lvl = 0; lvl < TIMER_LEVELS; lvl++) {
        uint32_t offset = lvl * TIMER_LEVEL_SIZE;

        /* Find the next set bit in pending_map for level */
        int32_t pos =
            timer_next_pending(base, offset, clock & TIMER_LEVEL_MASK);

        if (pos >= 0) {
            /* Absolute minimum this bucket represents */
            time_us_t sum = clock + (time_us_t) pos;
            sum <<= TIMER_LEVEL_SHIFT(lvl);

            if (sum < next)
                next = sum;

            /* If the bucket is within the current level's wraparound cycle,
             * it's guaranteed to expire before any timer in higher levels */
            time_us_t clock_lvl = clock & TIMER_CLOCK_MASK;
            if ((time_us_t) pos <=
                ((TIMER_CLOCK_FACTOR - clock_lvl) & TIMER_CLOCK_MASK))
                break;
        }

        /* Check next level, shift down to its granularity */
        clock >>= TIMER_CLOCK_SHIFT;
    }

    base->next_expiration_us = next;
    base->pending = (next != TIME_US_MAX);
    base->next_expiration_recalc = false;
}

static struct timer_base *timer_lock_base(struct timer *timer,
                                          enum irql *irql) {
    for (;;) {
        cpu_id_t cpu = timer_cpu_get(timer);
        struct timer_base *base = timer_base_for_cpu(timer->flags, cpu);

        *irql = spin_lock_irq_disable(&base->lock);
        if (timer_cpu_get(timer) == cpu)
            return base;

        spin_unlock(&base->lock, *irql);
    }
}

static void timer_base_run(struct timer_base *base) {
    struct hlist_head heads[TIMER_LEVELS];
    SPINLOCK_ASSERT_HELD(&base->lock);

    time_us_t time = time_get_us();
    enum irql irql;

    while (time >= base->clock && base->pending &&
           time >= base->next_expiration_us) {
        size_t levels = timer_collect_expired(base, heads);

        for (size_t i = 0; i < levels; i++)
            timer_expire_bucket(base, &heads[i], &irql);

        if (base->next_expiration_recalc)
            timer_recalc_next_expiration(base);
    }
}

void timer_add_on(struct timer *timer, cpu_id_t cpu) {
    enum irql irql;
    timer_cpu_set(timer, cpu);

    struct timer_base *base = timer_base_for_cpu(timer->flags, cpu);
    irql = spin_lock_irq_disable(&base->lock);

    timer_add_internal(base, timer);

    spin_unlock(&base->lock, irql);
}

void timer_add(struct timer *timer) {
    timer_add_on(timer, smp_core_id());
}

void timer_add_local(struct timer *timer) {
    timer->flags |= TIMER_FLAG_PINNED;
    timer_add(timer);
}

void timer_add_global(struct timer *timer) {
    timer->flags &= ~TIMER_FLAG_PINNED;
    timer_add(timer);
}

static bool timer_delete_internal(struct timer *timer, bool shutdown) {
    enum irql irql;
    struct timer_base *base = timer_lock_base(timer, &irql);

    bool pending = hlist_unhashed(&timer->hlist_node) == 0;
    if (pending) {
        timer_list_del(timer);

        uint32_t idx = timer_bucket_get(timer);
        if (hlist_empty(&base->buckets[idx]))
            bitmap_clear(base->pending_map, idx);

        base->next_expiration_recalc = true;
    }

    if (shutdown)
        timer->func = NULL;

    spin_unlock(&base->lock, irql);
    return pending;
}

bool timer_delete(struct timer *timer) {
    return timer_delete_internal(timer, false);
}

bool timer_shutdown(struct timer *timer) {
    return timer_delete_internal(timer, true);
}

static bool timer_sync_wait(struct timer *timer) {
    enum irql irql;
    struct timer_base *base = timer_lock_base(timer, &irql);

    while (base->running == timer) {
        spin_unlock(&base->lock, irql);
        timer_sync_wait_spin();
        base = timer_lock_base(timer, &irql);
    }

    spin_unlock(&base->lock, irql);
    return true;
}

bool timer_delete_sync(struct timer *timer) {
    bool pending = timer_delete(timer);
    timer_sync_wait(timer);
    return pending;
}

bool timer_shutdown_sync(struct timer *timer) {
    bool pending = timer_shutdown(timer);
    timer_sync_wait(timer);
    return pending;
}

bool timer_modify(struct timer *timer, time_us_t new_exp) {
    enum irql irql;
    struct timer_base *base = timer_lock_base(timer, &irql);

    bool pending = hlist_unhashed(&timer->hlist_node) == 0;
    if (pending) {
        timer_list_del(timer);
        uint32_t idx = timer_bucket_get(timer);
        if (hlist_empty(&base->buckets[idx])) {
            bitmap_clear(base->pending_map, idx);
        }
    }

    timer->expiration_us = new_exp;
    timer_add_internal(base, timer);

    spin_unlock(&base->lock, irql);
    return pending;
}

bool timer_modify_pending(struct timer *timer, time_us_t new_exp) {
    enum irql irql;
    struct timer_base *base = timer_lock_base(timer, &irql);

    bool pending = hlist_unhashed(&timer->hlist_node) == 0;
    if (pending) {
        timer_list_del(timer);
        uint32_t idx = timer_bucket_get(timer);
        if (hlist_empty(&base->buckets[idx])) {
            bitmap_clear(base->pending_map, idx);
        }

        timer->expiration_us = new_exp;
        timer_add_internal(base, timer);
    }

    spin_unlock(&base->lock, irql);
    return pending;
}

bool timer_modify_reduce(struct timer *timer, time_us_t new_exp) {
    enum irql irql;
    struct timer_base *base = timer_lock_base(timer, &irql);

    bool pending = hlist_unhashed(&timer->hlist_node) == 0;
    if (pending) {
        if (new_exp >= timer->expiration_us) {
            spin_unlock(&base->lock, irql);
            return true;
        }

        timer_list_del(timer);
        uint32_t idx = timer_bucket_get(timer);
        if (hlist_empty(&base->buckets[idx])) {
            bitmap_clear(base->pending_map, idx);
        }
    }

    timer->expiration_us = new_exp;
    timer_add_internal(base, timer);

    spin_unlock(&base->lock, irql);
    return pending;
}

void timer_base_reprogram_hardware(cpu_id_t cpu) {
    struct timer_percpu *pcpu = PERCPU_PTR_FOR_CPU(timer_percpu, cpu);
    struct clock_evdev *ced = pcpu->active_evdev;

    if (!ced || ced->state != CLOCK_EVDEV_STATE_ONESHOT)
        return;

    time_us_t next_us = MIN(pcpu->bases[TIMER_BASE_LOCAL].next_expiration_us,
                            pcpu->bases[TIMER_BASE_GLOBAL].next_expiration_us);

    /* TODO: _DEFERRED handling */
    if (next_us == (time_us_t) -1)
        return;

    time_us_t now_us = time_get_us();
    time_ns_t delta_ns =
        (next_us > now_us) ? MS_TO_NS(next_us - now_us) : ced->min_delta_ns;

    CLAMP(delta_ns, ced->min_delta_ns, ced->max_delta_ns);
    ced->set_next_event(ced, delta_ns);
}

void clock_evdev_irq_handler() {
    cpu_id_t cpu = smp_core_id();
    struct timer_percpu *pcpu = PERCPU_PTR(timer_percpu);

    for (int i = 0; i < TIMER_BASE_MAX; i++) {
        enum irql irql = spin_lock_irq_disable(&pcpu->bases[i].lock);
        timer_base_run(&pcpu->bases[i]);
        spin_unlock(&pcpu->bases[i].lock, irql);
    }

    timer_base_reprogram_hardware(cpu);
}

void timers_init() {
    struct clock_evdev_group *cedg =
        kassert(clock_evdev_group_search_for(clock_global.timer_clock_evdev));
    struct timer_percpu *iter;

    percpu_for_each(timer_percpu, iter, cpu) {
        struct clock_evdev *ced = clock_evdev_for_cpu(cedg, cpu);
        iter->active_evdev = ced;
    }
}
