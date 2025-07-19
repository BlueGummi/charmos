#pragma once
#include <boot/tss.h>
#include <stdatomic.h>
#include <stdbool.h>
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
extern struct core **global_cores;

static inline uint64_t get_this_core_id() {
    uint64_t id;
    asm volatile("movq %%gs:%c1, %0"
                 : "=r"(id)
                 : "i"(offsetof(struct core, id)));
    return id;
}

static inline void mark_self_in_interrupt(void) {
    global_cores[get_this_core_id()]->in_interrupt = true;
}

static inline void unmark_self_in_interrupt(void) {
    global_cores[get_this_core_id()]->in_interrupt = false;
}
