#include <acpi/lapic.h>
#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <int/idt.h>
#include <int/kb.h>
#include <mem/alloc.h>
#include <mp/core.h>
#include <mp/mp.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern void context_switch();
extern void page_fault_handler_wrapper();
extern void syscall_entry();
void page_fault_handler(uint64_t error_code, uint64_t fault_addr);
#define MAX_IDT_ENTRIES 256
static bool **idt_entry_used = NULL;
struct isr_entry **isr_table = NULL;

#include "isr_stubs.h"
#include "isr_vectors_array.h"

#define MAKE_THIN_HANDLER(handler_name, message)                               \
    void handler_name##_fault(void *frame) {                                   \
        (void) frame;                                                          \
        uint64_t core = get_this_core_id();                                     \
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
        uint64_t core = get_this_core_id();                                     \
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

void isr_common_entry(uint8_t vector, void *rsp) {
    uint8_t c = get_this_core_id();
    if (isr_table[c][vector].handler) {
        isr_table[c][vector].handler(isr_table[c][vector].ctx, vector, rsp);
    } else {
        k_printf("Unhandled ISR vector: %u\n", vector);
        while (1)
            asm volatile("hlt");
    }
}

void isr_timer_routine(void *ctx, uint8_t vector, void *rsp) {
    (void) ctx, (void) vector, (void) rsp;
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_EOI), 0);
    schedule();
}

void isr_register(uint8_t vector, isr_handler_t handler, void *ctx,
                  uint64_t c) {
    isr_table[c][vector].handler = handler;
    isr_table[c][vector].ctx = ctx;

    idt_set_gate(vector, (uint64_t) handler, 0x08, 0x8e, c);
}

struct idt_table *idts;
struct idt_ptr *idtps;

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags,
                  uint64_t ind) {
    struct idt_entry *idt = idts[ind].entries;

    isr_table[ind][num].handler = (void *) base;
    base = (uint64_t) isr_vectors[num];

    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].flags = flags;
    idt[num].reserved = 0;

    idt_entry_used[ind][num] = true;
}

static void set(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags,
                uint64_t ind) {
    struct idt_entry *idt = idts[ind].entries;

    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].flags = flags;
    idt[num].reserved = 0;

    idt_entry_used[ind][num] = true;
}

int idt_install_handler(uint8_t flags, void (*handler)(void), uint64_t core) {
    int entry = idt_alloc_entry();
    if (entry == -1)
        return -1;

    idt_set_gate(entry, (uint64_t) handler, 0x08, flags, core);
    return entry;
}

void idt_load(uint64_t ind) {
    idtps[ind].limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtps[ind].base = (uint64_t) &idts[ind];
    asm volatile("lidt %0" : : "m"(idtps[ind]));
}

void idt_alloc(uint64_t size) {
    idts = kmalloc(sizeof(struct idt_table) * size);
    idtps = kmalloc(sizeof(struct idt_ptr) * size);
    idt_entry_used = kmalloc(sizeof(bool *) * size);
    isr_table = kmalloc(sizeof(struct isr_entry *) * size);
    if (unlikely(!idts || !idtps || !idt_entry_used || !isr_table))
        k_panic("Could not allocate space for interrupt tables\n");

    for (uint64_t i = 0; i < size; i++) {
        idt_entry_used[i] = kzalloc(sizeof(bool) * IDT_ENTRIES);
        isr_table[i] = kzalloc(sizeof(struct isr_entry) * IDT_ENTRIES);
        if (unlikely(!idt_entry_used[i] || !isr_table[i]))
            k_panic("Could not allocate space for interrupt tables\n");
    }
}

int idt_alloc_entry_on_core(uint64_t c) {
    for (int i = 32; i < MAX_IDT_ENTRIES; i++) { // skip first 32: exceptions
        if (!idt_entry_used[c][i]) {
            idt_entry_used[c][i] = true;
            return i;
        }
    }
    return -1; // none available
}

int idt_alloc_entry(void) {
    return idt_alloc_entry_on_core(get_this_core_id());
}

void idt_set_alloc(int entry, uint64_t c, bool used) {
    idt_entry_used[c][entry] = used;
}

bool idt_is_installed(int entry) {
    return idt_entry_used[entry];
}

void idt_free_entry(int entry) {
    if (entry < 32 || entry >= MAX_IDT_ENTRIES)
        return;

    idt_entry_used[get_this_core_id()][entry] = false;
}

static void tlb_shootdown(void *ctx, uint8_t irq, void *rsp) {
    (void) ctx, (void) irq, (void) rsp;
    struct core *core = global_cores[get_this_core_id()];
    uintptr_t addr =
        atomic_load_explicit(&core->tlb_shootdown_page, memory_order_acquire);
    invlpg(addr);
    atomic_store_explicit(&core->tlb_shootdown_page, 0, memory_order_release);
}

void idt_install(uint64_t ind) {

    idt_set_gate(DIV_BY_Z_ID, (uint64_t) divbyz_fault, 0x08, 0x8E, ind);

    idt_set_gate(DEBUG_ID, (uint64_t) debug_fault, 0x08, 0x8E, ind);

    idt_set_gate(BREAKPOINT_ID, (uint64_t) breakpoint_fault, 0x08, 0x8E, ind);

    idt_set_gate(SSF_ID, (uint64_t) ss_handler, 0x08, 0x8E, ind);

    /*idt_set_gate(GPF_ID, (uint64_t) gpf_handler, 0x08, 0x8E, ind);
    idt_set_gate(DBF_ID, (uint64_t) double_fault_handler, 0x08, 0x8E, ind);
    idt_set_gate(PAGE_FAULT_ID, (uint64_t) page_fault_handler, 0x08, 0x8E,
    ind);*/

    idt_set_gate(TIMER_ID, (uint64_t) isr_timer_routine, 0x08, 0x8E, ind);

    set(KB_ID, (uint64_t) keyboard_handler, 0x08, 0x8E, ind);

    set(0x80, (uint64_t) syscall_entry, 0x2b, 0xee, ind);

    isr_register(TLB_SHOOTDOWN_ID, tlb_shootdown, NULL, ind);
    idt_load(ind);
}

void page_fault_handler(uint64_t error_code, uint64_t fault_addr) {
    //    uint64_t core = get_this_core_id();
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
