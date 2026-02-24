#include <console/printf.h>
#include <irq/irq.h>
#include <sch/sched.h>
#include <sync/spinlock.h>
#include <thread/thread.h>

static struct spinlock pf_lock = SPINLOCK_INIT;
enum irq_result page_fault_handler(void *context, uint8_t vector,
                                   struct irq_context *rsp) {
    (void) context, (void) vector, (void) rsp;
    struct thread *curr = thread_get_current();

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
    k_printf("  - Kernel stack 0x%lx -> 0x%lx\n", curr->stack,
             (uintptr_t) curr->stack + curr->stack_size);
    vaddr_t protector_base = (uintptr_t) curr->stack - PAGE_SIZE;
    vaddr_t protector_top = (uintptr_t) curr->stack;
    if (fault_addr >= protector_base && fault_addr <= protector_top)
        k_printf(
            "Likely stack overflow!! Fault occurred in protector page!!!\n");

    if (!(error_code & 0x04)) {
        spin_unlock_raw(&pf_lock);
        k_panic("KERNEL PAGE FAULT ON CORE %llu under thread %s\n",
                smp_core_id(), thread_get_current()->name);
        while (true) {
            disable_interrupts();
            wait_for_interrupt();
        }
    }
    spin_unlock_raw(&pf_lock);
    return IRQ_HANDLED;
}
