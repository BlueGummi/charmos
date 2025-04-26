#include <dbg.h>
#include <io.h>
#include <kb.h>
#include <pmm.h>
#include <printf.h>
#include <shutdown.h>
#include <stdint.h>
#include <vmalloc.h>
#include <vmm.h>

#define IDT_ENTRIES 256

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1

#define PAGE_FAULT_ID 0x0E
#define DIV_BY_Z_ID 0x0

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct idt_entry idt[IDT_ENTRIES];
struct idt_ptr idtp;

#define YELL                                                                   \
    do {                                                                       \
        k_printf("========== MANUAL HALT! ==========\n");                      \
        while (1) {                                                            \
            asm("hlt");                                                        \
        }                                                                      \
    } while (0)

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

void idt_install() {
    idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtp.base = (uint64_t) &idt;
    asm volatile("lidt %0" : : "m"(idtp));
}

static uint64_t read_cr2() {
    uint64_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}
static uint64_t read_cr3() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
__attribute__((interrupt)) void divbyz_fault(void *frame) {
    (void) frame;
    k_printf(
        "You fool! You bumbling babboon! You tried to divide a number by zero");
    k_printf(", why what an absolute goober you are!\n");
    k_shutdown();
}

__attribute__((interrupt)) void page_fault_handler(void *frame) {
    (void) frame;
    uint64_t cr3 = read_cr3();
    uint64_t cr2 = read_cr2();
    debug_print_registers();
    k_panic("Page fault! CR3 = 0x%zx\n              CR2 = 0x%zx", cr3, cr2);
}

void unmask_timer_and_keyboard() {
    uint8_t mask = inb(PIC1_DATA);

    mask &= ~(1 << 0);
    mask &= ~(1 << 1);

    outb(PIC1_DATA, mask);
}


void init_interrupts() {
    remap_pic();

    extern void timer_interrupt_handler();

    idt_set_gate(32, (uint64_t)timer_interrupt_handler, 0x08, 0x8E); // IRQ0
    idt_set_gate(33, (uint64_t)keyboard_handler, 0x08, 0x8E);         // IRQ1
    idt_set_gate(PAGE_FAULT_ID, (uint64_t)page_fault_handler, 0x08, 0x8E);

    idt_install();

    outb(0x43, 0x36);
    uint16_t divisor = 1193180 / 100;
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);

    unmask_timer_and_keyboard();

    asm volatile("sti");
}

