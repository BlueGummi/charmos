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
#define CRASH_WAIT_US 100000 /* Quiesce timeout per peer CPU (100ms) */
#define CRASH_SPIN_ONE_US 5
#define CRASH_MAX_DEPTH 2 /* Max recursive fault depth */

#define QEMU_EXIT_OK 0
#define QEMU_EXIT_FAIL 1
#define QEMU_EXIT_PANIC 2

static inline void qemu_exit(int code) {
    outb(0xf4, (uint8_t) code);
}

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
    CRASH_SOURCE_PANIC = 0,     /* Explicit panic() call */
    CRASH_SOURCE_ASSERT,        /* Kernel assertion failure */
    CRASH_SOURCE_KASAN,         /* Kernel Address Sanitizer check failure */
    CRASH_SOURCE_UBSAN,         /* Undefined Behavior Sanitizer failure */
    CRASH_SOURCE_NMI_WATCHDOG,  /* Watchdog / Liveness monitor hard stall */
    CRASH_SOURCE_CPU_EXCEPTION, /* Hardware CPU fault (GPF, Page Fault, #DF) */
    CRASH_SOURCE_NIGHTMARE,     /* Nightmare test harness failure / deadline */
};

#define CRASH_MSG_MAX 256

__noreturn void assert_impl_default(const char *file, int line,
                                    const char *func, const char *fmt, ...);

enum crash_format_flags {
    CRASH_FMT_RAW_SERIAL = 1 << 0,   /* Minimal serial printf */
    CRASH_FMT_VISUAL_PANES = 1 << 1, /* Dual-pane ANSI console report */
    CRASH_FMT_NDJSON = 1 << 2,       /* Machine-readable NDJSON stream */
    CRASH_FMT_DUMP_LOGS = 1 << 3,    /* In-memory circular log buffer dump */
    CRASH_FMT_PEER_CPUS = 1 << 4,    /* Quiesce and render peer CPU frames */

    /* Default formatting for standard kernel panics */
    CRASH_FMT_DEFAULT = CRASH_FMT_RAW_SERIAL | CRASH_FMT_VISUAL_PANES |
                        CRASH_FMT_NDJSON | CRASH_FMT_DUMP_LOGS |
                        CRASH_FMT_PEER_CPUS,

    /* Minimal formatting for super-early boot or nested recursive crashes */
    CRASH_FMT_MINIMAL = CRASH_FMT_RAW_SERIAL | CRASH_FMT_NDJSON,
};

struct crash_context {
    enum crash_source source;
    uint32_t formats; /* Bitmask of enum crash_format_flags */
    const char *file;
    int line;
    const char *func;
    const char *msg;
    const struct panic_regs *regs; /* NULL = capture caller's frame via asm */
    void *source_data;             /* Subsystem payload (e.g. kasan, stall) */
};

/* Core crash entry point */
__noreturn void crash(const struct crash_context *ctx);

/* Low-level helpers */
bool crash_cpu_is_owner(uint64_t id);
void crash_broadcast_nmi(void);
__noreturn void crash_nmi_handoff(void *p, struct irq_context *ctx);
void debug_print_stack(void);
