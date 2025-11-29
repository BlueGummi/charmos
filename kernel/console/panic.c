#include <acpi/lapic.h>
#include <asm.h>
#include <console/panic.h>
#include <console/printf.h>
#include <sleep.h>
#include <smp/core.h>

void panic_handler(struct panic_regs *regs) {
    disable_interrupts();

    k_printf("    [RAX]: %016lx  [RBX]: %016lx  [RCX]: %016lx\n", regs->rax,
             regs->rbx, regs->rcx);
    k_printf("    [RDX]: %016lx  [RSI]: %016lx  [RDI]: %016lx\n", regs->rdx,
             regs->rsi, regs->rdi);
    k_printf("    [RBP]: %016lx  [RSP]: %016lx\n\n", regs->rbp, regs->rsp);

    k_printf("    [ R8]: %016lx  [ R9]: %016lx  [R10]: %016lx\n", regs->r8,
             regs->r9, regs->r10);
    k_printf("    [R11]: %016lx  [R12]: %016lx  [R13]: %016lx\n", regs->r11,
             regs->r12, regs->r13);
    k_printf("    [R14]: %016lx  [R15]: %016lx\n\n", regs->r14, regs->r15);

    if (global.current_bootstage >= BOOTSTAGE_MID_MP) {
        broadcast_nmi_except(smp_core_id());
        sleep_ms(50);
    }
}
