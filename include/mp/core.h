#pragma once
#include <boot/stage.h>
#include <boot/tss.h>
#include <charmos.h>
#include <compiler.h>
#include <console/printf.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum core_state {
    IDLE,
    BUSY,
};

struct core {
    uint64_t id;
    struct thread *current_thread;
    enum core_state state;
    struct tss *tss;
    volatile atomic_uintptr_t tlb_shootdown_page;
    bool in_interrupt;
};

static inline uint64_t get_this_core_id() {
    uint64_t id;
    asm volatile("movq %%gs:%c1, %0"
                 : "=r"(id)
                 : "i"(offsetof(struct core, id)));
    return id;
}

static inline void mark_self_in_interrupt(void) {
    global.cores[get_this_core_id()]->in_interrupt = true;
}

static inline void unmark_self_in_interrupt(void) {
    global.cores[get_this_core_id()]->in_interrupt = false;
}
