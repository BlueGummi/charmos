#include <acpi/hpet.h>
#include <acpi/lapic.h>
#include <acpi/uacpi_interface.h> // mark handlers as installed
#include <asm.h>
#include <console/printf.h>
#include <int/idt.h>
#include <int/kb.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/dbg.h>
#include <sch/sched.h>
#include <stdint.h>

extern void context_switch();
extern void page_fault_handler_wrapper();

#define MAKE_THIN_HANDLER(handler_name, message)                               \
    void __attribute__((interrupt)) handler_name##_fault(void *frame) {        \
        (void) frame;                                                          \
        uint64_t core = get_sch_core_id();                                     \
        k_printf("\n=== " #handler_name " fault! ===\n");                      \
        k_printf("Message -> %s\n", message);                                  \
        k_panic("Core %u faulted\n", core);                                    \
        while (1) {                                                            \
            asm volatile("hlt");                                               \
        }                                                                      \
    }

#define MAKE_HANDLER(handler_name, mnemonic)                                   \
    extern void handler_name##_handler_wrapper();                              \
    void handler_name##_handler(uint64_t error_code) {                         \
        uint64_t core = get_sch_core_id();                                     \
        k_printf("\n=== " mnemonic " fault! ===\n");                           \
        k_printf("Error code: 0x%lx\n", error_code);                           \
        k_panic("Core %u faulted\n", core);                                    \
        while (1) {                                                            \
            asm volatile("hlt");                                               \
        }                                                                      \
    }

MAKE_THIN_HANDLER(divbyz, "Division by zero");
MAKE_THIN_HANDLER(debug, "Debug signal");
MAKE_THIN_HANDLER(breakpoint, "Breakpoint");
MAKE_HANDLER(gpf, "GPF");
MAKE_HANDLER(ss, "STACK SEGMENT FAULT");
MAKE_HANDLER(double_fault, "DOUBLE FAULT");

struct idt_table *idts;
struct idt_ptr *idtps;

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags,
                  uint64_t ind) {
    struct idt_entry *idt = idts[ind].entries;
    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].flags = flags;
    idt[num].reserved = 0;
}

void idt_set_and_mark(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags,
                      uint64_t ind) {
    idt_set_gate(num, base, sel, flags, ind);
    uacpi_mark_irq_installed(num);
}

void idt_load(uint64_t ind) {
    idtps[ind].limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtps[ind].base = (uint64_t) &idts[ind];
    asm volatile("lidt %0" : : "m"(idtps[ind]));
}

void idt_alloc(uint64_t size) {
    idts = kmalloc(sizeof(struct idt_table) * size);
    idtps = kmalloc(sizeof(struct idt_ptr) * size);
    if (!idts || !idtps)
        k_panic("Could not allocate space for IDT\n");
}

void idt_install(uint64_t ind) {

    idt_set_and_mark(DIV_BY_Z_ID, (uint64_t) divbyz_fault, 0x08, 0x8E, ind);

    idt_set_and_mark(DEBUG_ID, (uint64_t) debug_fault, 0x08, 0x8E, ind);

    idt_set_and_mark(BREAKPOINT_ID, (uint64_t) breakpoint_fault, 0x08, 0x8E,
                     ind);

    idt_set_and_mark(DOUBLEFAULT_ID, (uint64_t) double_fault_handler_wrapper,
                     0x08, 0x8E, ind);

    idt_set_and_mark(SSF_ID, (uint64_t) ss_handler_wrapper, 0x08, 0x8E, ind);

    idt_set_and_mark(GPF_ID, (uint64_t) gpf_handler_wrapper, 0x08, 0x8E, ind);

    idt_set_and_mark(PAGE_FAULT_ID, (uint64_t) page_fault_handler_wrapper, 0x08,
                     0x8E, ind);

    idt_set_and_mark(TIMER_ID, (uint64_t) context_switch, 0x08, 0x8E, ind);

    idt_set_and_mark(KB_ID, (uint64_t) keyboard_handler, 0x08, 0x8E, ind);

    idt_load(ind);
}

void page_fault_handler(uint64_t error_code, uint64_t fault_addr) {
    //    uint64_t core = get_sch_core_id();
    k_printf("\n=== PAGE FAULT ===\n");
    k_printf("Faulting Address (CR2): 0x%lx\n", fault_addr);
    //    k_printf("Core %u faulted\n", core);
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
    if (!(error_code & 0x04)) {
        k_panic("KERNEL PAGE FAULT\n");
        while (1) {
            asm("cli;hlt");
        }
    }
    /*    if (global_sched.active) {
            scheduler_rm_thread(&global_sched, global_sched.current);
        }*/
}
