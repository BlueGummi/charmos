#include <console/panic.h>
#define MAKE_HANDLER(handler_name, message)                                    \
    enum irq_result handler_name##_handler(void *ctx, uint8_t vector,          \
                                           struct irq_context *rsp) {          \
        (void) ctx, (void) vector, (void) rsp;                                 \
        uint64_t core = smp_core_id();                                         \
        printf("\n=== " #handler_name " fault! ===\n");                        \
        printf("Message -> %s\n", message);                                    \
        panic("Core %u faulted at %p", core, rsp->rip);                        \
        while (true) {                                                         \
            wait_for_interrupt();                                              \
        }                                                                      \
        return IRQ_HANDLED;                                                    \
    }

enum irq_result gpf_handler(void *ctx, uint8_t vector,
                            struct irq_context *rsp) {
    (void) ctx;
    (void) vector;

    uint64_t core = smp_core_id();
    uint64_t ec = rsp->error_code;

    printf("\n=== General Protection Fault ===\n");
    printf("Core:     %u\n", core);
    printf("RIP:      %p\n", rsp->rip);
    printf("RSP:      %p\n", rsp->rsp);
    printf("RBP:      %p\n", rsp->rbp);
    printf("CS:       0x%04x\n", rsp->cs);
    printf("SS:       0x%04x\n", rsp->ss);
    printf("RFLAGS:   0x%016lx\n", rsp->rflags);

    printf("\nError code: 0x%04lx\n", ec);
    if (ec == 0) {
        printf("  (no selector; likely a non-canonical access, "
               "null deref, or privilege violation)\n");
    } else {
        bool ext = ec & 1;
        uint8_t tbl = (ec >> 1) & 0x3;
        uint16_t index = (ec >> 3) & 0x1FFF;

        const char *tbl_name;
        switch (tbl) {
        case 0: tbl_name = "GDT"; break;
        case 1: tbl_name = "IDT"; break;
        case 2: tbl_name = "LDT"; break;
        case 3: tbl_name = "IDT"; break;
        default: tbl_name = "unknown"; break;
        }

        printf("  External:  %s\n", ext ? "yes (hardware)" : "no (software)");
        printf("  Table:     %s\n", tbl_name);
        printf("  Selector:  %u (0x%x)\n", index, index);
    }

    printf("\nRegister dump:\n");
    printf("  RAX=%016lx  RBX=%016lx\n", rsp->rax, rsp->rbx);
    printf("  RCX=%016lx  RDX=%016lx\n", rsp->rcx, rsp->rdx);
    printf("  RDI=%016lx  RSI=%016lx\n", rsp->rdi, rsp->rsi);
    printf("  R8 =%016lx  R9 =%016lx\n", rsp->r8, rsp->r9);
    printf("  R10=%016lx  R11=%016lx\n", rsp->r10, rsp->r11);
    printf("  R12=%016lx  R13=%016lx\n", rsp->r12, rsp->r13);
    printf("  R14=%016lx  R15=%016lx\n", rsp->r14, rsp->r15);

    uint64_t cr2, cr3;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    printf("\n  CR2=%016lx  CR3=%016lx\n", cr2, cr3);
    printf("\n--- Stack at fault RSP ---\n");
    debug_print_stack_from((uint64_t *) rsp->rsp, 0);

    panic("GPF on core %u at %p (error code 0x%lx)", core, rsp->rip, ec);

    while (true)
        wait_for_interrupt();

    return IRQ_HANDLED;
}

MAKE_HANDLER(divbyz, "Division by zero");
MAKE_HANDLER(debug, "Debug signal");
MAKE_HANDLER(breakpoint, "Breakpoint");
MAKE_HANDLER(ss, "STACK SEGMENT FAULT");
MAKE_HANDLER(double_fault, "DOUBLE FAULT");

enum irq_result panic_nmi_isr(void *ctx, uint8_t vector,
                              struct irq_context *rsp) {
    (void) ctx, (void) vector, (void) rsp;
    if (atomic_load(&global.panicked))
        panic_nmi_handoff(ctx, rsp);

    return IRQ_NONE;
}

enum irq_result hw_error_nmi_isr(void *ctx, uint8_t vector,
                                 struct irq_context *ictx) {
    uint8_t port61 = inb(0x61);
    if (port61 & 0xC0) /* bit 7 = RAM Parity, bit 6 = IOCHK, TODO: make a
                        * separate, clean hardware error/MCE subsystem */
        panic("Hardware / Memory Parity NMI Error (Port 0x61 = 0x%02x)",
              port61);

    return IRQ_NONE;
}

enum irq_result nop_handler(void *ctx, uint8_t vector,
                            struct irq_context *rsp) {
    (void) ctx, (void) vector, (void) rsp;
    return IRQ_HANDLED;
}

enum irq_result dpc_handler(void *ctx, uint8_t vector,
                            struct irq_context *rsp) {
    scheduler_mark_self_needs_run_dpcs(true);
    (void) ctx, (void) vector, (void) rsp;
    return IRQ_HANDLED;
}
