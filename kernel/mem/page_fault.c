#include <console/printf.h>
#include <irq/irq.h>
#include <sync/spinlock.h>

static struct spinlock pf_lock = SPINLOCK_INIT;
enum irq_result page_fault_handler(void *context, uint8_t vector,
                                   struct irq_context *rsp) {
    (void) context, (void) vector, (void) rsp;

    uint64_t error_code = UINT64_MAX;
    paddr_t rsp_phys = vmm_get_phys_unsafe((vaddr_t) rsp);

    if (rsp_phys != (paddr_t) -1) {

        uint64_t *stack = (uint64_t *) rsp;
        error_code = stack[15];
    }

    uint64_t fault_addr;

    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

    spin_lock_raw(&pf_lock);
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
        spin_unlock_raw(&pf_lock);
        k_panic("KERNEL PAGE FAULT ON CORE %llu\n", smp_core_id());
        while (true) {
            disable_interrupts();
            wait_for_interrupt();
        }
    }
    spin_unlock_raw(&pf_lock);
    return IRQ_HANDLED;
}
