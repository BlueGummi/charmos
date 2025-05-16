#include <dbg.h>
#include <idt.h>
#include <io.h>
#include <kb.h>
#include <pmm.h>
#include <printf.h>
#include <sched.h>
#include <shutdown.h>
#include <stdint.h>
#include <vmalloc.h>
#include <vmm.h>

struct idt_entry idt[IDT_ENTRIES];
struct idt_ptr idtp;

void remap_pic() {
    uint8_t a1, a2;

    a1 = inb(PIC1_DATA); // Save master PIC mask
    a2 = inb(PIC2_DATA); // Save slave PIC mask

    outb(PIC1_COMMAND, 0x11); // Start init
    outb(PIC2_COMMAND, 0x11);

    outb(PIC1_DATA, 0x20); // Master offset: 0x20 (32)
    outb(PIC2_DATA, 0x28); // Slave offset: 0x28 (40)

    outb(PIC1_DATA, 0x04); // Tell master about slave on IRQ2
    outb(PIC2_DATA, 0x02); // Tell slave its cascade identity

    outb(PIC1_DATA, 0x01); // 8086/88 mode
    outb(PIC2_DATA, 0x01);

    // Restore saved masks
    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].flags = flags;
    idt[num].reserved = 0;
}

void idt_load() {
    idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtp.base = (uint64_t) &idt;
    asm volatile("lidt %0" : : "m"(idtp));
}

__attribute__((interrupt)) void divbyz_fault(void *frame) {
    (void) frame;
    k_printf(
        "You fool! You bumbling babboon! You tried to divide a number by zero");
    k_printf(", why what an absolute goober you are!\n");
    k_shutdown();
}

void page_fault_handler(uint64_t error_code, uint64_t fault_addr) {
    k_printf("\n=== PAGE FAULT ===\n");
    k_printf("Faulting Address (CR2): 0x%lx\n", fault_addr);
    k_printf("Error Code: 0x%lx\n", error_code);
    k_printf("  - Page not Present (P): %s\n",
             (error_code & 0x01) ? "Yes" : "No");
    k_printf("  - Write Access (W/R): %s\n",
             (error_code & 0x02) ? "Write" : "Read");
    k_printf("  - User Mode (U/S): %s\n",
             (error_code & 0x04) ? "User" : "Supervisor");
    k_printf("  - Reserved Bit Set (RSVD): %s\n",
             (error_code & 0x08) ? "Yes" : "No");
    k_printf("  - Instruction Fetch (I/D): %s\n",
             (error_code & 0x10) ? "Yes" : "No");
    k_printf("  - Protection Key Violation (PK): %s\n",
             (error_code & 0x20) ? "Yes" : "No");
    if (global_sched.active) {
        scheduler_rm_thread(&global_sched, global_sched.current);
    }
}

void unmask_timer_and_keyboard() {
    uint8_t mask = inb(PIC1_DATA);

    mask &= ~(1 << 0);
    mask &= ~(1 << 1);

    outb(PIC1_DATA, mask);
}

void idt_install() {
    remap_pic();

    extern void context_switch();
    extern void page_fault_handler_wrapper();

    idt_set_gate(32, (uint64_t) context_switch, 0x08, 0x8E); // IRQ0
    idt_set_gate(33, (uint64_t) keyboard_handler, 0x08, 0x8E);        // IRQ1
    idt_set_gate(PAGE_FAULT_ID, (uint64_t) page_fault_handler_wrapper, 0x08,
                 0x8E);

    idt_load();

    outb(0x43, 0x36);
    uint16_t divisor = 1193180 / 100;
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);

    unmask_timer_and_keyboard();
}
