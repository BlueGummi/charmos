/* @title: Timer */
#pragma once
#include <structures/hlist.h>
#include <time/time.h>
#include <types/types.h>

/* timer_flags: 32 bit bitflags
 *
 *      ┌───────────────────────────────────────────────────────┐
 * Bits │ 31..28 27..24 23..20 19..16 15..12  11..8  7..4  3..0 │
 * Use  │  %%%%   %%%%   %%PM   ID##   ####    ####  ####  #### │
 *      └───────────────────────────────────────────────────────┘
 *
 * I - IRQ timer - not executed in DPC
 * D - Deferrable (Only activates when CPU non-idle)
 * M - Migrating
 * P - Pinned
 *
 * %%% - Bucket idx (if present, up to 1023)
 * ### - CPU target for this timer
 *
 * A - Unused (available)
 * * - Unused (unavailable)
 *
 */

#define TIMER_FLAG_CPU_MASK 0x0003FFFF
#define TIMER_FLAG_CPU(c) ((c) & TIMER_FLAG_CPU_MASK)

#define TIMER_FLAG_BUCKET_MASK 0x3FF
#define TIMER_FLAG_BUCKET_SHIFT 22
#define TIMER_FLAG_BUCKET(b)                                                   \
    (((b) & TIMER_FLAG_BUCKET_MASK) << TIMER_FLAG_BUCKET_SHIFT)

enum timer_flags {
    TIMER_FLAG_NONE = 0,
    TIMER_FLAG_DEFERRABLE = 1 << 18,
    TIMER_FLAG_IRQ = 1 << 19,
    TIMER_FLAG_MIGRATING = 1 << 20,
    TIMER_FLAG_PINNED = 1 << 21,
};

struct timer {
    struct hlist_node hlist_node;
    time_us_t expiration_us;
    void (*func)(struct timer *timer);
    enum timer_flags flags;

    void *data;
};

#define TIMER_DECLARE(n, fn, ...)                                              \
    struct timer n = (struct timer) {                                          \
        .func = fn                                                             \
    }

void timer_add(struct timer *timer);
void timer_add_on(struct timer *timer, cpu_id_t cpu);

/* Wrappers around `add()` with flag setting */
void timer_add_local(struct timer *timer);
void timer_add_global(struct timer *timer);

bool timer_modify(struct timer *timer, time_us_t new);
bool timer_modify_pending(struct timer *timer, time_us_t new);

/* Only modify if `new` is nearer than `expiration` */
bool timer_modify_reduce(struct timer *timer, time_us_t new);

/* `_sync()` functions wait for the handler to finish,
 * and non-`_sync()` functions do not */
bool timer_delete(struct timer *timer);
bool timer_delete_sync(struct timer *timer);
bool timer_shutdown(struct timer *timer);
bool timer_shutdown_sync(struct timer *timer);

void timers_init();

static inline time_us_t timer_delta_us(time_us_t delta_us) {
    return time_get_us() + delta_us;
}

static inline void timer_bucket_set(struct timer *timer, uint32_t bucket) {
    timer->flags =
        (timer->flags & ~TIMER_FLAG_BUCKET_MASK) | TIMER_FLAG_BUCKET(bucket);
}

static inline uint32_t timer_bucket_get(struct timer *timer) {
    return (timer->flags >> TIMER_FLAG_BUCKET_SHIFT) & TIMER_FLAG_BUCKET_MASK;
}

static inline void timer_cpu_set(struct timer *timer, cpu_id_t cpu) {
    timer->flags = (timer->flags & ~TIMER_FLAG_CPU_MASK) | TIMER_FLAG_CPU(cpu);
}

static inline cpu_id_t timer_cpu_get(struct timer *timer) {
    return timer->flags & TIMER_FLAG_CPU_MASK;
}
