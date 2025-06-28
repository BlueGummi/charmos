#include <asm.h>
#include <console/printf.h>
#include <int/idt.h>
#include <int/irq.h>

static struct irq_entry irq_table[IRQ_MAX] = {0};

int irq_install(uint8_t irq, irq_handler_t handler, void *ctx) {
    if (irq_table[irq].installed)
        return -1;

    irq_table[irq].installed = true;
    irq_table[irq].handler = handler;
    irq_table[irq].ctx = ctx;

    idt_set_gate(irq + 32, (uint64_t) handler, 0x08, 0x8E, get_core_id());

    return 0;
}

int irq_free(uint8_t irq) {
    if (!irq_table[irq].installed)
        return -1;

    irq_table[irq].installed = false;
    irq_table[irq].handler = NULL;
    irq_table[irq].ctx = NULL;

    idt_set_gate(irq + 32, 0, 0, 0, get_sch_core_id());
    return 0;
}

void irq_dispatch(uint8_t irq) {
    if (!irq_table[irq].installed)
        return;

    irq_table[irq].handler(irq_table[irq].ctx);
}

bool irq_is_installed(uint8_t irq) {
    return irq_table[irq].installed;
}

void irq_set_installed(uint8_t irq, bool state) {
    irq_table[irq].installed = state;
}

int irq_alloc_free_vector(void) {
    for (int i = 32; i < IRQ_MAX; ++i) {
        if (!irq_table[i].installed)
            return i;
    }
    return -1;
}
