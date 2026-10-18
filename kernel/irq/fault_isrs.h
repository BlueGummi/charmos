#include <console/crash.h>
#include <console/panic.h>

#define MAKE_HANDLER(handler_name, message)                                    \
    enum irq_result handler_name##_handler(struct irq_context *rsp,            \
                                           void *ctx) {                        \
        cc_ignore(ctx);                                                        \
        struct crash_regs pregs;                                               \
        irq_context_to_crash_regs(rsp->regs, &pregs);                          \
        char msg[CRASH_MSG_MAX];                                               \
        snprintf(msg, sizeof(msg), "CPU %u fault: " message " at %p",          \
                 (uint32_t) smp_id_raw(), (void *) rsp->regs->rip);            \
        crash_full(&(struct crash_context) {                                   \
            .source = CRASH_SOURCE_CPU_EXCEPTION,                              \
            .formats = CRASH_FMT_DEFAULT,                                      \
            .file = __FILE__,                                                  \
            .line = __LINE__,                                                  \
            .func = #handler_name "_handler",                                  \
            .msg = msg,                                                        \
            .regs = &pregs,                                                    \
        });                                                                    \
        return IRQ_HANDLED;                                                    \
    }

enum irq_result gpf_handler(struct irq_context *ictx, void *ctx) {
    cc_ignore(ctx);

    uint64_t core = smp_id_raw();
    struct irq_registers *rsp = ictx->regs;
    uint64_t ec = rsp->error_code;

    printf("\n=== General Protection Fault ===\n");
    printf("Core:     %u\n", (uint32_t) core);
    printf("RIP:      %p\n", (void *) rsp->rip);
    printf("RSP:      %p\n", (void *) rsp->rsp);
    printf("RBP:      %p\n", (void *) rsp->rbp);
    printf("CS:       0x%04x\n", (uint16_t) rsp->cs);
    printf("SS:       0x%04x\n", (uint16_t) rsp->ss);
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

    struct crash_regs pregs;
    irq_context_to_crash_regs(rsp, &pregs);
    char msg[CRASH_MSG_MAX];
    snprintf(msg, sizeof(msg), "GPF on core %u at %p (error code 0x%lx)",
             (uint32_t) core, (void *) rsp->rip, ec);
    crash_full(&(struct crash_context) {
        .source = CRASH_SOURCE_CPU_EXCEPTION,
        .formats = CRASH_FMT_DEFAULT,
        .file = __FILE__,
        .line = __LINE__,
        .func = __func__,
        .msg = msg,
        .regs = &pregs,
    });

    return IRQ_HANDLED;
}

MAKE_HANDLER(divbyz, "Division by zero");
MAKE_HANDLER(debug, "Debug signal");
MAKE_HANDLER(breakpoint, "Breakpoint");
MAKE_HANDLER(ss, "STACK SEGMENT FAULT");
MAKE_HANDLER(double_fault, "DOUBLE FAULT");

enum irq_result panic_nmi_isr(struct irq_context *rsp, void *ctx) {
    cc_ignore(rsp, ctx);
    if (atomic_load(&global.panicked)) {
        if (crash_cpu_is_owner(smp_id_raw()))
            return IRQ_HANDLED;

        crash_nmi_handoff(ctx, rsp->regs);
    }

    return IRQ_NONE;
}

enum irq_result hw_error_nmi_isr(struct irq_context *ictx, void *ctx) {
    cc_ignore(ctx, ictx);
    uint8_t port61 = inb(0x61);
    if (port61 & 0xC0) {
        char msg[CRASH_MSG_MAX];
        snprintf(msg, sizeof(msg),
                 "Hardware / Memory Parity NMI Error (Port 0x61 = 0x%02x)",
                 port61);
        crash_full(&(struct crash_context) {
            .source = CRASH_SOURCE_CPU_EXCEPTION,
            .formats = CRASH_FMT_DEFAULT,
            .file = __FILE__,
            .line = __LINE__,
            .func = __func__,
            .msg = msg,
        });
    }

    return IRQ_NONE;
}

enum irq_result nop_handler(struct irq_context *ictx, void *ctx) {
    cc_ignore(ctx, ictx);
    return IRQ_HANDLED;
}

enum irq_result dpc_handler(struct irq_context *rsp, void *ctx) {
    scheduler_mark_self_needs_run_dpcs(true);
    cc_ignore(ctx, rsp);
    return IRQ_HANDLED;
}
