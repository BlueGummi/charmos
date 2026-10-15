/* @title: Thread Enumerations and Types */
#pragma once
#include <mem/page.h>
#include <stdarg.h>
#include <stdint.h>

struct thread;
struct thread_create_params;
struct cpu_context;

typedef void (*thread_entry_fn_t)(void *arg);
typedef uint8_t thread_act_reason_t; /* Polymorphic type:
                                      * all the THREAD_*_REASONs
                                      * are uint8_t, and often, we
                                      * may need just a generic reason.
                                      *
                                      * This is that. */

/* Both ASAN and the lock validator are very eager to consume
 * stack memory, so we'll give threads 16 times as many pages
 * if either of those happen to be on, and this should
 * give enough headroom for when both are on too */
#if defined(DEBUG_ASAN) || defined(DEBUG_LOCK_CHK)
#define THREAD_STACK_SIZE (PAGE_SIZE * 16)
#else
#define THREAD_STACK_SIZE (PAGE_SIZE * 4)
#endif

#define THREAD_STACKS_HEAP_START 0xFFFFF10000000000ULL
#define THREAD_STACKS_HEAP_END 0xFFFFF20000000000ULL

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
    THREAD_WAIT_UNINTERRUPTIBLE, /* Only object matching/satisfaction ends it */
    THREAD_WAIT_INTERRUPTIBLE,   /* APCs OK, alerts end the wait */
};

enum thread_wait_status : uint8_t {
    THREAD_WAIT_SATISFIED,
    THREAD_WAIT_ALERTED,
};

/* thread_flags: 32 bit bitflags:
 *
 *      ┌───────────────────────────────────────────────────────────┐
 * Bits │ 31..28  27..24  23..20  19..16  15..12  11..8  7..4  3..0 │
 * Use  │  AAAA    ****    ****    ****    ****    *adJ  jRWY  DEFP │
 *      └───────────────────────────────────────────────────────────┘
 * P - Pinned - Thread is pinned to current CPU
 * F - Flexible RT - realtime scheduler related stuff
 * E - Executing APC
 * D - Dying
 * Y - Yielded after a wait (block, sleep)
 * W - Available
 * r - Realtime fault tolerance
 * j - Joinable - someone holds a join reference on this thread
 * J - Joined - a join is in progress or consumed the join reference
 * d - Diagnostic - Diagnostics enabled for this thread in debug
 * a - Delivering APCs - In the APC delivery code
 *
 * A - Unused (Available)
 * * - Unused (Unavailable)
 *
 */
enum thread_flags : uint32_t {
    THREAD_FLAG_PINNED = 1,
    THREAD_FLAG_FLEXIBLE_RT = 1 << 1,
    THREAD_FLAG_EXECUTING_APC = 1 << 2,
    THREAD_FLAG_DYING = 1 << 3,
    THREAD_FLAG_YIELDED = 1 << 4,
    THREAD_FLAG_RT_FAULT_TOLERANCE = 1 << 6,
    THREAD_FLAG_JOINABLE = 1 << 7,
    THREAD_FLAG_JOINED = 1 << 8,

    THREAD_FLAG_DIAG = 1 << 9,

    /* This is for when inside the APC subsystem, whereas
     * THREAD_FLAG_EXECUTING_APC is for an actual APC callback */
    THREAD_FLAG_DELIVERING_APCS = 1 << 10,

};

enum thread_prio_class : uint8_t {
    THREAD_PRIO_CLASS_BACKGROUND = 0, /* Background thread */
    THREAD_PRIO_CLASS_TIMESHARE = 1,  /* Timesharing thread */
    THREAD_PRIO_CLASS_REALTIME = 2,   /* Realtime thread */
    THREAD_PRIO_CLASS_URGENT = 3,     /* Urgent thread - ran before RT */
};
#define THREAD_PRIO_CLASS_COUNT (4)

/* Different enums are used for the little
 * bit of type safety since different ringbuffers
 * are used to keep track of different reasons */
enum thread_resume_reason : uint8_t {
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

/* Used in condvars, totally separate from thread_resume_reason */
enum wake_reason {
    WAKE_REASON_NONE = 0,    /* No reason specified */
    WAKE_REASON_SIGNAL = 1,  /* Signal from something */
    WAKE_REASON_TIMEOUT = 2, /* Timeout */
};

struct thread_class_donation {
    struct thread *target;
    enum thread_prio_class class : 2;
    bool active;
};

#define THREAD_CLASS_DONATION_EMPTY                                            \
    ((struct thread_class_donation) {.target = NULL,                           \
                                     .class = THREAD_PRIO_CLASS_BACKGROUND,    \
                                     .active = false})

struct thread *thread_create_full(char *name, thread_entry_fn_t entry,
                                  struct thread_create_params *params,
                                  va_list args) cc_printf_like(1, 0);
