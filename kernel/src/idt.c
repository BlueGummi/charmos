#include <stdint.h>
#include <system/dbg.h>
#include <system/io.h>
#include <system/kb.h>
#include <system/pmm.h>
#include <system/printf.h>
#include <system/shutdown.h>
#include <system/vmm.h>

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

#define YELL                                              \
    do {                                                  \
        k_printf("========== MANUAL HALT! ==========\n"); \
        while (1) {                                       \
            asm("hlt");                                   \
        }                                                 \
    } while (0)

void remap_pic() {
    outb(PIC1_COMMAND, 0x11); // Start initialization of master PIC
    outb(PIC2_COMMAND, 0x11); // Start initialization of slave PIC

    outb(PIC1_DATA, 0x20); // Set master PIC offset to 0x20
    outb(PIC2_DATA, 0x28); // Set slave PIC offset to 0x28

    outb(PIC1_DATA, 0x04); // Master PIC IRQ2 connected to slave
    outb(PIC2_DATA, 0x02); // Slave PIC connected to IRQ2 on master

    outb(PIC1_DATA, 0x01); // Set 8086 mode for master PIC
    outb(PIC2_DATA, 0x01); // Set 8086 mode for slave PIC

    outb(PIC1_DATA, 0xFD); // Enable IRQ1 (keyboard) on master PIC
    outb(PIC2_DATA, 0xFF); // Disable all interrupts on slave PIC
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

static inline uint64_t read_cr2() {
    uint64_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}
static inline uint64_t read_cr3() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
__attribute__((interrupt)) void divbyz_fault(void *frame) {
    (void) frame;
    k_printf("You fool! You bumbling babboon! You tried to divide a number by zero");
    k_printf(", why what an absolute goober you are!\n");
    k_panic("The system will power off now\n");
    k_shutdown();
}

__attribute__((interrupt)) void page_fault_handler(void *frame) {
    (void) frame;
    uint64_t cr3 = read_cr3();
    uint64_t cr2 = read_cr2();
    k_panic("Page fault! CR3 = 0x%zx\n              CR2 = 0x%zx", cr3, cr2);
    vmm_map_page(cr2, sub_offset((uint64_t) pmm_alloc_page()), PAGING_PRESENT | PAGING_XD);
    /*    uint64_t fault_addr;
        asm volatile("mov %%cr2, %0" : "=r" (fault_addr));

        if (!(error_code & PAGE_PRESENT)) {
            if (is_valid_fault_address(fault_addr)) {
                uint64_t *phys = pmm_alloc_page();
                map_page(fault_addr, *phys, PAGE_PRESENT | PAGE_WRITE);
                return;
            }
        }

        k_panic("Unhandled page fault at 0x%llx, error code: 0x%llx", fault_addr, error_code);*/
}

void init_interrupts() {
    remap_pic();

    idt_set_gate(33, (uint64_t) keyboard_handler, 0x08, 0x8E);
    idt_set_gate(PAGE_FAULT_ID, (uint64_t) page_fault_handler, 0x08, 0x8E);
    idt_set_gate(DIV_BY_Z_ID, (uint64_t) divbyz_fault, 0x08, 0x8E);

    idt_install();

    asm volatile("sti");
}
