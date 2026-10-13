/* @title: Crash Engine */
#pragma once
#include <asm.h>
#include <atomic.h>
#include <compiler/core.h>
#include <linker/symbols.h>
#include <sch/irql.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <time/time.h>
#include <types/types.h>

struct spinlock;
struct qspinlock;
struct rwlock;
struct mutex;
struct thread;
struct irq_registers;
struct irq_context;

#define CRASH_REG_COUNT 20

enum qemu_exit_codes {
    QEMU_EXIT_OK = 0,
    QEMU_EXIT_FAIL = 1,
    QEMU_EXIT_PANIC = 2,
};

struct crash_regs {
    union {
        struct {
            uint64_t rip, rflags, cr2, cr3;
            uint64_t rax, rbx, rcx, rdx, rbp, rdi, rsi;
            uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
            uint64_t rsp;
        };

        uint64_t regs[CRASH_REG_COUNT];
    };
};

ct_assert_struct_size_eq(crash_regs, CRASH_REG_COUNT * 8);

enum crash_code {
    CRASH_CODE_GENERIC,
};

enum crash_source {
    CRASH_SOURCE_PANIC = 0,     /* panic() call */
    CRASH_SOURCE_ENFORCE,       /* enforce() failure */
    CRASH_SOURCE_KASAN,         /* ASAN check failure */
    CRASH_SOURCE_UBSAN,         /* UBSAN failure */
    CRASH_SOURCE_NMI_WATCHDOG,  /* Watchdog / Liveness monitor hard stall */
    CRASH_SOURCE_CPU_EXCEPTION, /* Hardware CPU fault */
    CRASH_SOURCE_NIGHTMARE,     /* Nightmare test harness failure */
    CRASH_SOURCE_LOCK_CHK, /* Lock validator order / dependency violation */
};

enum crash_format_flags {
    CRASH_FMT_RAW_SERIAL = 1 << 0,   /* Minimal serial printf */
    CRASH_FMT_VISUAL_PANES = 1 << 1, /* Dual pane ANSI console report */
    CRASH_FMT_NDJSON = 1 << 2,       /* NDJSON stream */
    CRASH_FMT_DUMP_LOGS = 1 << 3,    /* circular log buffer dump */
    CRASH_FMT_PEER_CPUS = 1 << 4,    /* Quiesce and render peer CPU frames */

    /* Default formatting */
    CRASH_FMT_DEFAULT = CRASH_FMT_RAW_SERIAL | CRASH_FMT_VISUAL_PANES |
                        CRASH_FMT_NDJSON | CRASH_FMT_DUMP_LOGS |
                        CRASH_FMT_PEER_CPUS,

    /* Minimal formatting for early boot or nested crashes */
    CRASH_FMT_MINIMAL = CRASH_FMT_RAW_SERIAL | CRASH_FMT_NDJSON,
};

enum crash_hook_flags {
    CRASH_HOOK_DEFAULT = 0,
    CRASH_HOOK_FACILITY = 1 << 0, /* Facility granularity.
                                   * This means that the crash_code the
                                   * crash_hook contains is only checked for the
                                   * upper word facility
                                   */

    CRASH_HOOK_NO_UNWIND = 1 << 1, /* By default, the crash handler will make
                                    * a best-effort attempt to unwind: it'll
                                    * try to safely drop locks, exit RCU
                                    * read-side critical sections, although
                                    * memory may still leak (this is dependent
                                    * on the facility implementation).
                                    *
                                    * NO_UNWIND allows this behavior to be
                                    * skipped. This is because in certain cases,
                                    * the unwinding cannot happen (e.g. lock
                                    * checking is disabled), and also because
                                    * the stale state not unwinding
                                    * leaves can be used as a postmortem
                                    * for state verification.
                                    */

};

enum crash_unwind_type {
    CRASH_UNWIND_NONE, /* should not be reachable */
    CRASH_UNWIND_RCU,
    CRASH_UNWIND_MUTEX,
    CRASH_UNWIND_RWLOCK,
    CRASH_UNWIND_SPINLOCK,
    CRASH_UNWIND_QSPINLOCK,

    CRASH_UNWIND_MAX,
};

struct crash_payload {
    enum crash_code code;
    void *data;
    uintptr_t params[4];
};

struct report_target;
struct crash_facility {
    uint16_t prefix;
    const char *name;
    const char *desc;
    uint16_t hookable_threshold; /* We use this to state that if the delta
                                  * >= this, we treat it as hookable. So,
                                  * if it's left unset, it'll be 0 and
                                  * all codes are hookable. We do this
                                  * because setting some high bit can cause
                                  * the enum to count funny as it would
                                  * bump up from there
                                  *
                                  * this is basically "First hookable code"
                                  */
    const char *(*const to_str)(uint16_t delta);
    void (*const dump)(uint16_t delta, struct crash_payload pl);

    void (*const emit_ndjson)(uint16_t delta, struct crash_payload pl);
};

/* The idea here:
 *
 * Each thread has a fixed pool of unwind nodes, with a list that stores
 * the currently active nodes, which are unwound in LIFO order.
 *
 * The reason why it's a pool and not a stack is because you can have code
 * such as
 *
 * mutex_lock(&lock);
 * rcu_read_lock();
 *
 * mutex_unlock(&lock);
 * rcu_read_unlock();
 *
 * and with a naive stack, the semantics break here/become tricky because
 * the thread would have to reorganize the stack, whereas a list_head
 * can just be yanked out of the middle.
 *
 * Thus, the unwind node enqueue path is:
 *
 * (1) try to reserve one
 * (2) list_add_tail it
 *
 * And dequeue becomes:
 *
 * (1) find the pointer to the current thing in question
 * (2) dequeue it
 *
 * RCU is even simpler: the first rcu_read_lock gets a node,
 * and every subsequent lock bumps a counter, dec'ing the counter
 * upon unlock, and the crash path simply rcu_read_unlock's it
 * that many times.
 */

struct crash_unwind_node_data {
    uintptr_t arg;
    union {
        void *ptr;
        size_t rcu_lock_times;
        enum irql irql;
        uintptr_t raw;
    };
};

struct crash_unwind_node {
    enum crash_unwind_type type;
    struct list_head list;
    struct crash_unwind_node_data data;
};

#define CRASH_UNWIND_NODES 128
struct crash_unwind_perthread {
    struct list_head free_list;
    struct list_head in_use; /* LIFO */
    struct crash_unwind_node nodes[CRASH_UNWIND_NODES];
};

struct crash_perthread {
    struct crash_unwind_perthread unwind;
    struct list_head crash_hooks; /* struct crash_hook */
    bool unwinding;
    bool in_hook;
    jmp_buf env;
};

/*
 * The idea of crash hooks:
 *
 * A hook can hook into a specific code, which is checked
 * against a facility to verify if it is hookable.
 *
 * They are hooked PER-THREAD, so as to not introduce strange
 * non-determinism bugs and the chance that the crash hook
 * registration path itself can crash (would be problematic)
 * due to synchronization violations.
 *
 * CRASH_CODE_GENERIC is not hookable
 */
struct crash_hook {
    char *name;
    struct list_head list;
    enum crash_code code;
    enum crash_source source_mask;
};

struct crash_context {
    struct crash_payload payload;
    enum crash_source source;
    enum crash_format_flags formats;
    const char *stmt;
    const char *file;
    int line;
    const char *func;
    const char *msg;
    const struct crash_regs *regs; /* NULL = capture caller's frame via asm */
    void *source_data;
};

#define CRASH_WAIT_US MS_TO_US(500) /* Quiesce timeout per peer CPU */
#define CRASH_SPIN_ONE_US 5
#define CRASH_MAX_DEPTH 2 /* Max recursive fault depth */
#define CRASH_MSG_MAX 256

#define CRASH_PAYLOAD(c, d) ((struct crash_payload) {.code = c, .data = d})
#define CRASH_PARAMS(c, p0, p1, p2, p3)                                        \
    ((struct crash_payload) {.code = (c),                                      \
                             .params = {(uintptr_t) (p0), (uintptr_t) (p1),    \
                                        (uintptr_t) (p2), (uintptr_t) (p3)}})

#define CRASH_CODE_TO_PAYLOAD(c) ((struct crash_payload) {.code = c})
#define CRASH_CODE_CREATE(pre, del)                                            \
    ({ ((((int) (pre)) << 16) | (((int) (del)) & 0xFFFF)); })

#define CRASH_CODE_GET_FACILITY(c) ({ (((c)) >> 16) & 0xFFFF; })

#define CRASH_CODE_GET_DELTA(c) ({ (((c)) & 0xFFFF); })

#define CRASH_CODE_PREFIX(n) ((__crash_facility_##n).prefix)
#define CRASH_CODE_DELTA_START (1)
#define CRASH_CODE(n, d) CRASH_CODE_CREATE(CRASH_CODE_PREFIX(n), d)

#define CRASH_FACILITY(n) __crash_facility_##n
#define CRASH_FACILITY_EXTERN(n)                                               \
    extern struct crash_facility __crash_facility_##n
#define CRASH_FACILITY_DEFINE(n, ...)                                          \
    LINKER_SECTION_OBJECT(struct crash_facility, crash_facilities)             \
    __crash_facility_##n = {.name = #n, __VA_ARGS__}

LINKER_SECTION_EXTERN(struct crash_facility, crash_facilities);

cc_noreturn void enforce_impl_msg(const char *file, int line, const char *func,
                                  const char *prefix, const char *expr,
                                  const char *fmt, ...) cc_printf_like(6, 7);
cc_noreturn void enforce_impl_code(const char *file, int line, const char *func,
                                   const char *prefix, const char *expr,
                                   enum crash_code code, const char *fmt, ...)
    cc_printf_like(7, 8);
cc_noreturn void enforce_impl_code_only(const char *file, int line,
                                        const char *func, const char *prefix,
                                        const char *expr, enum crash_code code);

cc_noreturn void crash_full(const struct crash_context *ctx);

bool crash_cpu_is_owner(uint64_t id);
void crash_broadcast_nmi(void);
void crash_facilities_init(void);
const char *crash_code_from_facility_to_str(enum crash_code code);
void crash_nmi_handoff(void *p, struct irq_registers *irqc);
void debug_print_stack(void);
void crash_facility_printf(const char *fmt, ...) cc_printf_like(1, 2);
void crash_perthread_init(struct thread *t);
void crash_unwind(void);

/* Enter/exit pairs */
void crash_unwind_enter_rcu(void);
void crash_unwind_exit_rcu(void);
void crash_unwind_enter_mutex(struct mutex *m);
void crash_unwind_exit_mutex(struct mutex *m);
void crash_unwind_enter_rwlock(struct rwlock *r);
void crash_unwind_exit_rwlock(struct rwlock *r);
void crash_unwind_enter_spinlock(struct spinlock *s, enum irql old);
void crash_unwind_exit_spinlock(struct spinlock *s);
void crash_unwind_enter_qspinlock(struct qspinlock *q, enum irql old);
void crash_unwind_exit_qspinlock(struct qspinlock *q);

static inline void qemu_exit(int code) {
    outb(0xf4, (uint8_t) code);
}

STRINGIFY_DECLARE_ENUM(crash_code) {
    switch (stringify_in) {
    case CRASH_CODE_GENERIC: return "Generic";
    default: return crash_code_from_facility_to_str(stringify_in);
    }
}
