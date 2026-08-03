#pragma once
#include <asm.h>
#include <compiler.h>
#include <limine.h>
#include <types/types.h>

struct irq_context;
void debug_print_stack();
extern void panic_entry();
void panic_broadcast_nmi();
__noreturn void panic_nmi_handoff(void *p, struct irq_context *ctx);
typedef __noreturn void (*panic_handler_t)(const char *file, int line,
                                           const char *func, const char *fmt,
                                           ...);

enum panic_type {
    PANIC_LOCK,
    PANIC_IRQ,
};

static inline void qemu_exit(int code) {
    outb(0xf4, (uint8_t) code);
}

/* TODO: */
struct panic_hook {
    _Atomic panic_handler_t impl;
    bool enabled;
};

#define PANIC_REG_COUNT 20
#define PANIC_WAIT_US 100000 /* How long to wait for a CPU to quiesce */
#define PANIC_SPIN_ONE_US 5

struct panic_regs {
    union {
        struct {
            uint64_t rip, rflags, cr2, cr3;
            uint64_t rax, rbx, rcx, rdx, rbp, rdi, rsi;
            uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
            uint64_t rsp;
        };

        uint64_t regs[PANIC_REG_COUNT];
    };
};

static_assert_struct_size_eq(panic_regs, PANIC_REG_COUNT * 8);

__noreturn void panic_impl_default(const char *file, int line, const char *func,
                                   const char *fmt, ...);

#define panic(fmt, ...)                                                        \
    panic_impl_default(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/* TODO: */
#define panic_with_impl(hook, fmt, ...)                                        \
    hook.impl(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define oops(fmt, ...)
