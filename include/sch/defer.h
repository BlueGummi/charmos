#include <stdbool.h>
#include <stdint.h>
#pragma once

typedef void (*dpc_t)(void *arg);

struct deferred_event {
    uint64_t timestamp_ms;
    dpc_t callback;
    void *arg;
    struct deferred_event *next;
};

void defer_init(void);

/* can only fail from allocation fail */
bool defer_enqueue(dpc_t func, void *arg, uint64_t delay_ms);
