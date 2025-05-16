#include <printf.h>
#include <sched.h>
#include <stdint.h>

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
