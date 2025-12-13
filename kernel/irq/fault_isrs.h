#define MAKE_HANDLER(handler_name, message)                                    \
    enum irq_result handler_name##_handler(void *ctx, uint8_t vector,          \
                                           struct irq_context *rsp) {          \
        (void) ctx, (void) vector, (void) rsp;                                 \
        uint64_t core = smp_core_id();                                         \
        k_printf("\n=== " #handler_name " fault! ===\n");                      \
        k_printf("Message -> %s\n", message);                                  \
        k_panic("Core %u faulted\n", core);                                    \
        while (true) {                                                         \
            wait_for_interrupt();                                              \
        }                                                                      \
        return IRQ_HANDLED;                                                    \
    }

MAKE_HANDLER(divbyz, "Division by zero");
MAKE_HANDLER(debug, "Debug signal");
MAKE_HANDLER(breakpoint, "Breakpoint");
MAKE_HANDLER(gpf, "GPF");
MAKE_HANDLER(ss, "STACK SEGMENT FAULT");
MAKE_HANDLER(double_fault, "DOUBLE FAULT");

enum irq_result nmi_isr(void *ctx, uint8_t vector, struct irq_context *rsp) {
    (void) ctx, (void) vector, (void) rsp;
    if (atomic_load(&global.panicked)) {
        disable_interrupts();
        k_printf("    [CPU %u] Halting due to system panic\n", smp_core_id());
        while (true)
            wait_for_interrupt();
    }
    return IRQ_HANDLED;
}

enum irq_result nop_handler(void *ctx, uint8_t vector,
                            struct irq_context *rsp) {
    (void) ctx, (void) vector, (void) rsp;
    return IRQ_HANDLED;
}
