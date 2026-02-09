#include <acpi/lapic.h>
#include <asm.h>
#include <global.h>
#include <console/panic.h>
#include <console/printf.h>
#include <logo.h>
#include <sleep.h>
#include <smp/core.h>
#include <stdarg.h>
#include <sync/spinlock.h>
#include <time.h>

void panic_handler(struct panic_regs *regs) {
    (void) regs;
    disable_interrupts();

    /*
    k_printf("    [RAX]: %016lx  [RBX]: %016lx  [RCX]: %016lx\n", regs->rax,
             regs->rbx, regs->rcx);
    k_printf("    [RDX]: %016lx  [RSI]: %016lx  [RDI]: %016lx\n", regs->rdx,
             regs->rsi, regs->rdi);
    k_printf("    [RBP]: %016lx  [RSP]: %016lx\n\n", regs->rbp, regs->rsp);

    k_printf("    [ R8]: %016lx  [ R9]: %016lx  [R10]: %016lx\n", regs->r8,
             regs->r9, regs->r10);
    k_printf("    [R11]: %016lx  [R12]: %016lx  [R13]: %016lx\n", regs->r11,
             regs->r12, regs->r13);
    k_printf("    [R14]: %016lx  [R15]: %016lx\n\n", regs->r14, regs->r15);*/

    if (global.current_bootstage >= BOOTSTAGE_MID_MP) {
        panic_broadcast(smp_core_id());
        sleep_ms(50);
    }
}

static struct spinlock panic_lock = SPINLOCK_INIT;

void k_panic_impl(const char *file, int line, const char *func, const char *fmt,
                  ...) {
    disable_interrupts();

    spin_lock_raw(&panic_lock);
    atomic_store(&global.panicked, true);

    k_printf("\n" EIGHTY_LINES "\n");
    k_printf("\n                                [" ANSI_BG_RED
             "KERNEL PANIC" ANSI_RESET "] @ time %llu\n",
             time_get_ms());
    k_printf(ANSI_RED "%s\n" ANSI_RESET, OS_LOGO_PANIC_CENTERED);

    panic_entry();

    k_printf("    [" ANSI_BRIGHT_BLUE "AT" ANSI_RESET " ");
    time_print_current();
    k_printf("]\n");

    k_printf("    [" ANSI_BRIGHT_GREEN "FROM" ANSI_RESET "] " ANSI_GREEN
             "%s" ANSI_RESET ":" ANSI_GREEN "%d" ANSI_RESET ":" ANSI_CYAN
             "%s()" ANSI_RESET "\n"
             "    [" ANSI_YELLOW "MESSAGE" ANSI_RESET "] ",
             file, line, func);

    va_list args;
    va_start(args, fmt);
    v_k_printf(NULL, fmt, args);
    va_end(args);

    debug_print_stack();

    k_printf("\n" EIGHTY_LINES "\n");

    spin_unlock_raw(&panic_lock);
    while (true)
        wait_for_interrupt();
}
