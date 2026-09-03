/* @title: Crash Engine */
#pragma once
#include <asm.h>
#include <compiler.h>
#include <linker/symbols.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time/time.h>
#include <types/types.h>

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

static_assert_struct_size_eq(crash_regs, CRASH_REG_COUNT * 8);

enum crash_code {
    CRASH_CODE_GENERIC,
};

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
    const char *(*const to_str)(uint16_t delta);
    void (*const dump)(uint16_t delta, struct crash_payload pl);

    void (*const emit_ndjson)(uint16_t delta, struct crash_payload pl);
};

struct crash_context {
    struct crash_payload payload;
    enum crash_source source;
    enum crash_format_flags formats;
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
#define CRASH_FACILITY_DECLARE(n, ...)                                         \
    LINKER_SECTION_OBJECT(struct crash_facility, crash_facilities)             \
    __crash_facility_##n = {.name = #n, __VA_ARGS__}

LINKER_SECTION_DEFINE(struct crash_facility, crash_facilities);

__noreturn void assert_impl_default(struct crash_payload payload,
                                    const char *file, int line,
                                    const char *func, const char *fmt, ...);

__noreturn void assert_impl_assertion(struct crash_payload payload,
                                      const char *file, int line,
                                      const char *func, const char *prefix,
                                      const char *assertion, const char *fmt,
                                      ...);
__noreturn void crash_full(const struct crash_context *ctx);

bool crash_cpu_is_owner(uint64_t id);
void crash_broadcast_nmi(void);
void crash_facilities_init(void);
const char *crash_code_from_facility_to_str(enum crash_code code);
__noreturn void crash_nmi_handoff(void *p, struct irq_context *ctx);
void debug_print_stack(void);
void crash_facility_printf(const char *fmt, ...);

static inline void qemu_exit(int code) {
    outb(0xf4, (uint8_t) code);
}

static inline const char *crash_code_to_str(enum crash_code code) {
    switch (code) {
    case CRASH_CODE_GENERIC: return "Generic";
    default: return crash_code_from_facility_to_str(code);
    }
}
