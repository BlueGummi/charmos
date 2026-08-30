/* @title: Kernel Panic Interface */
#pragma once
#include <console/crash.h>

__noreturn void panic_impl_default(const char *file, int line, const char *func,
                                   const char *fmt, ...);

__noreturn void panic_impl_with_regs(const struct crash_regs *regs,
                                     const char *file, int line,
                                     const char *func, const char *fmt, ...);

#define panic(fmt, ...)                                                        \
    panic_impl_default(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define panic_with_regs(regs, fmt, ...)                                        \
    panic_impl_with_regs(regs, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/* Compatibility aliases */
static inline bool panic_cpu_is_owner(uint64_t id) {
    return crash_cpu_is_owner(id);
}

static inline void panic_broadcast_nmi(void) {
    crash_broadcast_nmi();
}

#define panic_nmi_handoff crash_nmi_handoff

#define oops(fmt, ...)
