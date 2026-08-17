/* @title: Completion */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>
#include <time/time.h>

/* Basically just struct semaphore with completion semantics :^) */
#define COMPLETION_INIT_IRQ_DISABLE true
#define COMPLETION_INIT_NORMAL false

struct completion {
    _Atomic uint32_t done;
    bool irq_disable;

    struct spinlock lock;
    struct condvar cv;
};

#define COMPLETION_INIT(irq_dis)                                               \
    (struct completion) {                                                      \
        .done = ATOMIC_VAR_INIT(0), .irq_disable = (irq_dis),                  \
        .lock = SPINLOCK_INIT,                                                 \
        .cv = {                                                                \
            .waiters = THREAD_QUEUE_INIT,                                      \
            .irq_disable = (irq_dis),                                          \
        },                                                                     \
    }

void completion_init(struct completion *c, bool irq_disable);
void completion_reinit(struct completion *c);
void completion_wait(struct completion *c);
bool completion_wait_timeout(struct completion *c, time_ms_t timeout_ms);
bool completion_try_wait(struct completion *c);
void complete(struct completion *c);
void complete_all(struct completion *c);
bool completion_done(struct completion *c);
