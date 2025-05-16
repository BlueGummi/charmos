#include <pmm.h>
#include <printf.h>
#include <stdint.h>
#include <uacpi/event.h>
#include <uacpi/status.h>
#include <uacpi/uacpi.h>
#include <vmm.h>

extern uint64_t a_rsdp;

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    *out_rsdp_address = a_rsdp;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    return vmm_map_region(addr, (uint64_t) len, PAGING_PRESENT | PAGING_WRITE);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    for (uint64_t i = 0; i < (uint64_t) len / 4096; i++) {
        vmm_unmap_page((uint64_t) addr);
    }
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *data) {
    k_printf(">> UACPI LOG: LEVEL = %d\n", (int) level);
    k_printf(">> %s\n", data);
}
