#pragma once
#include <asm.h>
#include <limine.h>

void debug_print_stack();
extern void panic_entry();

static inline void qemu_exit(int code) {
    outb(0xf4, ((code << 1) | 1) & 0xFF);
}

struct panic_regs {
    uint64_t rsp;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
};

#define TEN_LINES "=========="
#define EIGHTY_LINES                                                           \
    TEN_LINES TEN_LINES TEN_LINES TEN_LINES TEN_LINES TEN_LINES TEN_LINES      \
        TEN_LINES

void panic_impl(const char *file, int line, const char *func, const char *fmt,
                ...);

#define panic(fmt, ...)                                                      \
    panic_impl(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
