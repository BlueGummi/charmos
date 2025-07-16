#include <stdint.h>
#pragma once

typedef void (*defer_func_t)(void *arg);

struct deferred_event {
    uint64_t timestamp_ms;
    defer_func_t callback;
    void *arg;
    struct deferred_event *next;
};

void defer_init(void);
void defer_enqueue(defer_func_t func, void *arg, uint64_t delay_ms);
