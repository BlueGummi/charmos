#pragma once
#include <asm.h>
#include <compiler.h>
#include <limine.h>

void debug_print_stack();
extern void panic_entry();
void panic_broadcast_nmi();
typedef __noreturn void (*panic_handler_t)(const char *file, int line,
                                           const char *func, const char *fmt,
                                           ...);

static inline void qemu_exit(int code) {
    outb(0xf4, ((code << 1) | 1) & 0xFF);
}

struct panic_hook {
    _Atomic panic_handler_t impl;
    bool enabled;
};

struct panic_regs {
    uint64_t rsp;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
};

__noreturn void panic_impl_default(const char *file, int line, const char *func,
                                   const char *fmt, ...);

#define panic(fmt, ...)                                                        \
    panic_impl_default(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define panic_with_impl(hook, fmt, ...)                                        \
    hook.impl(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define oops(fmt, ...)
