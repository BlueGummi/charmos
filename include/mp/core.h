#include <sch/thread.h>
#include <spin_lock.h>
#include <stdint.h>

enum core_state {
    IDLE,
    BUSY,
};

struct core {
    uint64_t id;
    struct thread *current_thread;
    enum core_state state;
};

#pragma once
