#include <acpi/lapic.h>
#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <int/idt.h>
#include <int/kb.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <mp/core.h>
#include <mp/mp.h>
#include <sch/apc.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <types/rcu.h>

extern void context_switch();
extern void page_fault_handler_wrapper();
extern void syscall_entry();
#define MAX_IDT_ENTRIES 256
static bool idt_entry_used[MAX_IDT_ENTRIES] = {0};
static struct isr_entry isr_table[MAX_IDT_ENTRIES] = {0};

#include "isr_stubs.h"
#include "isr_vectors_array.h"

#define MAKE_HANDLER(handler_name, message)                                    \
    void handler_name##_handler(void *ctx, uint8_t vector, void *rsp) {        \
        (void) ctx, (void) vector, (void) rsp;                                 \
        uint64_t core = get_this_core_id();                                    \
        k_printf("\n=== " #handler_name " fault! ===\n");                      \
        k_printf("Message -> %s\n", message);                                  \
        k_panic("Core %u faulted\n", core);                                    \
        while (1) {                                                            \
            wait_for_interrupt();                                              \
        }                                                                      \
    }

MAKE_HANDLER(divbyz, "Division by zero");
MAKE_HANDLER(debug, "Debug signal");
MAKE_HANDLER(breakpoint, "Breakpoint");
MAKE_HANDLER(gpf, "GPF");
MAKE_HANDLER(ss, "STACK SEGMENT FAULT");
MAKE_HANDLER(double_fault, "DOUBLE FAULT");

void isr_common_entry(uint8_t vector, void *rsp) {
    enum irql old = irql_raise(IRQL_HIGH_LEVEL);
    mark_self_in_interrupt();
    unmark_self_idle();

    if (isr_table[vector].handler) {
        isr_table[vector].handler(isr_table[vector].ctx, vector, rsp);
    } else {
        k_printf("Unhandled ISR vector: %u\n", vector);
        while (1)
            wait_for_interrupt();
    }

    rcu_mark_quiescent();
    unmark_self_in_interrupt();
    irql_lower(old);
}

void isr_timer_routine(void *ctx, uint8_t vector, void *rsp) {
    (void) ctx, (void) vector, (void) rsp;
    lapic_write(LAPIC_REG_EOI, 0);

    /* Doing this as the `schedule()` will go to another thread */
    unmark_self_in_interrupt();

    schedule();
}

/* Literally a no-op. Used to break out of "wait for interrupt" loops */
static void nop_handler(void *ctx, uint8_t vector, void *rsp) {
    (void) ctx, (void) vector, (void) rsp;
}

void panic_isr(void *ctx, uint8_t vector, void *rsp) {
    (void) ctx, (void) vector, (void) rsp;
    if (global.panic_in_progress) {
        disable_interrupts();
        k_printf("    [CPU %u] Halting due to system panic\n",
                 get_this_core_id());
        while (1)
            wait_for_interrupt();
    }
}

void isr_register(uint8_t vector, isr_handler_t handler, void *ctx) {
    isr_table[vector].handler = handler;
    isr_table[vector].ctx = ctx;

    idt_set_gate(vector, (uint64_t) handler, 0x08, 0x8e);
}

static struct idt_table idts = {0};
static struct idt_ptr idtps = {0};

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    struct idt_entry *idt = idts.entries;

    isr_table[num].handler = (void *) base;
    base = (uint64_t) isr_vectors[num];

    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].flags = flags;
    idt[num].reserved = 0;

    idt_entry_used[num] = true;
}

static void set(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    struct idt_entry *idt = idts.entries;

    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].flags = flags;
    idt[num].reserved = 0;

    idt_entry_used[num] = true;
}

int idt_install_handler(uint8_t flags, void (*handler)(void)) {
    int entry = idt_alloc_entry();
    if (entry == -1)
        return -1;

    idt_set_gate(entry, (uint64_t) handler, 0x08, flags);
    return entry;
}

void idt_load(void) {
    idtps.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtps.base = (uint64_t) &idts;
    asm volatile("lidt %0" : : "m"(idtps));
}

int idt_alloc_entry() {
    for (int i = 32; i < MAX_IDT_ENTRIES; i++) { // skip first 32: exceptions
        if (!idt_entry_used[i]) {
            idt_entry_used[i] = true;
            return i;
        }
    }
    return -1; // none available
}

void idt_set_alloc(int entry, bool used) {
    idt_entry_used[entry] = used;
}

bool idt_is_installed(int entry) {
    return idt_entry_used[entry];
}

void idt_free_entry(int entry) {
    if (entry < 32 || entry >= MAX_IDT_ENTRIES)
        return;

    idt_entry_used[entry] = false;
}

static struct spinlock pf_lock = SPINLOCK_INIT;
static void page_fault_handler(void *context, uint8_t vector, void *rsp) {
    (void) context, (void) vector;

    uint64_t *stack = (uint64_t *) rsp;
    uint64_t error_code = stack[15];
    uint64_t fault_addr;

    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

    spin_lock_no_cli(&pf_lock);
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

    if (!(error_code & 0x04)) {
        spin_unlock_no_cli(&pf_lock);
        k_panic("KERNEL PAGE FAULT ON CORE %llu\n", get_this_core_id());
        while (1) {
            disable_interrupts();
            wait_for_interrupt();
        }
    }
    spin_unlock_no_cli(&pf_lock);
    /*    if (global_sched.active) {
            scheduler_rm_thread(&global_sched, global_sched.current);
        }*/
}

void idt_init() {
    isr_register(IRQ_DIV_BY_Z, divbyz_handler, NULL);
    isr_register(IRQ_DEBUG, debug_handler, NULL);
    isr_register(IRQ_BREAKPOINT, breakpoint_handler, NULL);

    isr_register(IRQ_SSF, ss_handler, NULL);
    isr_register(IRQ_GPF, gpf_handler, NULL);
    isr_register(IRQ_DBF, double_fault_handler, NULL);
    isr_register(IRQ_PAGE_FAULT, page_fault_handler, NULL);

    isr_register(IRQ_TIMER, isr_timer_routine, NULL);
    isr_register(IRQ_PANIC, panic_isr, NULL);
    isr_register(IRQ_TLB_SHOOTDOWN, tlb_shootdown, NULL);
    isr_register(IRQ_NOP, nop_handler, NULL);

    set(0x80, (uint64_t) syscall_entry, 0x2b, 0xee);
    idt_load();
}
