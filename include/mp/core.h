#include <sch/thread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <tss.h>

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
};

#pragma once
