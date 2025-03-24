void remap_pic();

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

__attribute__((interrupt)) void keyboard_handler(void *a);

void idt_install();

void init_interrupts();
