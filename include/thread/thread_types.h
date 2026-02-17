/* @title: Thread Enumerations and Types */
#pragma once
#include <mem/page.h>
#include <stdarg.h>
#include <stdint.h>

struct thread;
struct cpu_context;

#define THREAD_STACK_SIZE (PAGE_SIZE * 4)

enum thread_state : uint8_t {
    THREAD_STATE_IDLE_THREAD, /* Specifically the idle thread */
    THREAD_STATE_READY,   /* Thread is ready to run but not currently running */
    THREAD_STATE_RUNNING, /* Thread is currently executing */
    THREAD_STATE_BLOCKED, /* Waiting on I/O, lock, or condition */
    THREAD_STATE_SLEEPING, /* Temporarily not runnable */
    THREAD_STATE_ZOMBIE, /* Finished executing but hasn't been reaped it yet */
    THREAD_STATE_TERMINATED, /* Fully done, can be cleaned up */
    THREAD_STATE_HALTED,     /* Thread manually suspended */
};

enum thread_wait_type : uint8_t {
    THREAD_WAIT_NONE,
    THREAD_WAIT_UNINTERRUPTIBLE, /* Cannot be interrupted by anything
                                    besides the wake source */

    THREAD_WAIT_INTERRUPTIBLE, /* Can be interrupted */
};

enum thread_flags : uint8_t {
    THREAD_FLAGS_NO_STEAL = 1, /* Do not migrate between cores */
};

enum thread_prio_class : uint8_t {
    THREAD_PRIO_CLASS_BACKGROUND = 0, /* Background thread */
    THREAD_PRIO_CLASS_TIMESHARE = 1,  /* Timesharing thread */
    THREAD_PRIO_CLASS_RT = 2,         /* Realtime thread */
    THREAD_PRIO_CLASS_URGENT = 3,     /* Urgent thread - ran before RT */
};
#define THREAD_PRIO_CLASS_COUNT (4)

/* Different enums are used for the little
 * bit of type safety since different ringbuffers
 * are used to keep track of different reasons */
enum thread_wake_reason : uint8_t {
    THREAD_WAKE_REASON_BLOCKING_IO = 1,
    THREAD_WAKE_REASON_BLOCKING_MANUAL = 2,
    THREAD_WAKE_REASON_SLEEP_TIMEOUT = 3,
    THREAD_WAKE_REASON_SLEEP_MANUAL = 4,
};

enum thread_block_reason : uint8_t {
    THREAD_BLOCK_REASON_IO = 5,
    THREAD_BLOCK_REASON_MANUAL = 6,
};

enum thread_sleep_reason : uint8_t {
    THREAD_SLEEP_REASON_MANUAL = 7,

};

/* Used in condvars, totally separate from thread_wake_reason */
enum wake_reason {
    WAKE_REASON_NONE = 0,    /* No reason specified */
    WAKE_REASON_SIGNAL = 1,  /* Signal from something */
    WAKE_REASON_TIMEOUT = 2, /* Timeout */
};

/* headers that #include this header should be allowed to execute this function
 */
struct thread *thread_create_internal(char *name, void (*entry_point)(void *),
                                      void *arg, size_t stack_size,
                                      va_list args);
