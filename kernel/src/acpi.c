#include <stdint.h>
#include <system/vmm.h>
#include <system/pmm.h>
#include <system/printf.h>
#include <uacpi/status.h>
#include <uacpi/uacpi.h>
#include <uacpi/event.h>

extern uint64_t a_rsdp;

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    *out_rsdp_address = a_rsdp;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    uint64_t first_addr = (uint64_t) pmm_alloc_page();
    for (uint64_t i = 0; i < (uint64_t) len / 4096; i++) {
        if (i == 0) {
            vmm_map_page((uint64_t) addr, sub_offset(first_addr), PAGING_PRESENT | PAGING_WRITE);
        } else {
            vmm_map_page((uint64_t) addr, sub_offset((uint64_t) pmm_alloc_page()), PAGING_PRESENT | PAGING_WRITE);
        }
    }
    return (void *) first_addr;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    for (uint64_t i = 0; i < (uint64_t) len / 4096; i++) {
        vmm_unmap_page((uint64_t) addr);
    }
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *data) {
    k_printf("UACPI LOG: LEVEL = %d ", (int) level);
    k_printf(data);
}
