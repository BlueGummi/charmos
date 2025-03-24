#include <stdint.h>
#include <system/page.h>
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
            paging_map_cr3((uint64_t) addr, first_addr, PAGING_X86_64_PRESENT | PAGING_X86_64_WRITE);
        } else {
            paging_map_cr3((uint64_t) addr, (uint64_t) pmm_alloc_page(), PAGING_X86_64_PRESENT | PAGING_X86_64_WRITE);
        }
    }
    return (void *) first_addr;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    for (uint64_t i = 0; i < (uint64_t) len / 4096; i++) {
        paging_unmap_cr3((uint64_t) addr);
    }
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *data) {
    k_printf("UACPI LOG: LEVEL = %d ", (int) level);
    k_printf(data);
}
