/* @title: Crash Engine */
#pragma once
#include <asm.h>
#include <compiler.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <types/types.h>

struct irq_context;

#define CRASH_REG_COUNT 20

enum qemu_exit_codes {
    QEMU_EXIT_OK = 0,
    QEMU_EXIT_FAIL = 1,
    QEMU_EXIT_PANIC = 2,
};

struct panic_regs {
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

static_assert_struct_size_eq(panic_regs, CRASH_REG_COUNT * 8);

enum crash_source {
    CRASH_SOURCE_PANIC = 0,     /* panic() call */
    CRASH_SOURCE_ASSERT,        /* assertion failure */
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

struct crash_context {
    enum crash_source source;
    enum crash_format_flags formats;
    const char *file;
    int line;
    const char *func;
    const char *msg;
    const struct panic_regs *regs; /* NULL = capture caller's frame via asm */
    void *source_data;
};

#define CRASH_WAIT_US 100000 /* Quiesce timeout per peer CPU (100ms) */
#define CRASH_SPIN_ONE_US 5
#define CRASH_MAX_DEPTH 2 /* Max recursive fault depth */
#define CRASH_MSG_MAX 256

__noreturn void assert_impl_default(const char *file, int line,
                                    const char *func, const char *fmt, ...);
__noreturn void crash(const struct crash_context *ctx);

bool crash_cpu_is_owner(uint64_t id);
void crash_broadcast_nmi(void);
__noreturn void crash_nmi_handoff(void *p, struct irq_context *ctx);
void debug_print_stack(void);

static inline void qemu_exit(int code) {
    outb(0xf4, (uint8_t) code);
}
