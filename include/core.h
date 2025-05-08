#include <stdint.h>
#include <spin_lock.h>
#include <task.h>

enum core_state {
    IDLE,
    BUSY,
};

struct core {
    uint64_t id;
    struct task *current_task;
    enum core_state state;
};