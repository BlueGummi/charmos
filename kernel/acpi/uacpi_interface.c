#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <console/printf.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <spin_lock.h>
#include <stdint.h>
#include <uacpi/event.h>
#include <uacpi/internal/types.h>
#include <uacpi/platform/arch_helpers.h>
#include <uacpi/status.h>
#include <uacpi/uacpi.h>

static irq_entry_t irq_table[IDT_ENTRIES];

extern uint64_t a_rsdp;

extern uint64_t tsc_freq;

#define DEFINE_IRQ_HANDLER(n)                                                  \
    __attribute__((interrupt)) void irq##n##_entry(void *);                    \
    __attribute__((interrupt)) void irq##n##_entry(void *) {                   \
        if (irq_table[n].installed) {                                          \
            irq_table[n].handler(irq_table[n].ctx);                            \
        }                                                                      \
    }
#include "irq_handlers.h"

void irq_common_handler(uint8_t irq_num) {

    if (irq_num >= MAX_IRQ || !irq_table[irq_num].installed)
        return;

    irq_table[irq_num].handler(irq_table[irq_num].ctx);
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {

    if (a_rsdp == 0) {
        k_printf("no rsdp set\n");
        return UACPI_STATUS_INTERNAL_ERROR;
    }
    *out_rsdp_address = a_rsdp;
    return UACPI_STATUS_OK;
}

extern uintptr_t vmm_map_top;

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    return vmm_map_phys(addr, len);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    vmm_unmap_virt(addr, len);
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *data) {
    switch (level) {
    case UACPI_LOG_ERROR: k_printf(">> UACPI ERROR: %s\n", data);
    case UACPI_LOG_DEBUG:
    case UACPI_LOG_INFO:
    case UACPI_LOG_WARN:
    default: break;
    }
}

void *uacpi_kernel_alloc(uacpi_size size) {
    void *x = kmalloc(size);
    return x;
}

void uacpi_kernel_free(void *mem) {
    kfree(mem);
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len,
                                 uacpi_handle *out_handle) {
    if (!out_handle || len == 0) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uacpi_io_handle *handle =
        (uacpi_io_handle *) kmalloc(sizeof(uacpi_io_handle));
    if (!handle) {
        k_panic("Failed to allocate I/O handle");
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    handle->base = base;
    handle->len = len;
    handle->valid = true;

    *out_handle = (uacpi_handle) handle;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle h) {
    if (!h)
        return;

    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    handle->valid = false;
    kfree(handle);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {

    return (rdtsc() * 1000000000ull) / tsc_freq;
}

void uacpi_kernel_stall(uacpi_u8 usec) {

    uint64_t start = rdtsc();
    uint64_t target = start + ((tsc_freq / 1000000ull) * usec);

    while (rdtsc() < target)
        __asm__ volatile("pause");
}

void uacpi_kernel_sleep(uacpi_u64 msec) {

    for (uacpi_u64 i = 0; i < msec * 10; ++i)
        uacpi_kernel_stall(100);
}

void (*isr_trampolines[])(void *) = {
#define X(n) [n] = irq##n##_entry,
#include "irq_list.h"
#undef X
};

void uacpi_mark_irq_installed(uint8_t irq) { // this is used in idt.c to avoid overwriting
    irq_table[irq].installed = true;
}

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle) {

    if (irq >= IDT_ENTRIES)
        return UACPI_STATUS_INVALID_ARGUMENT;

    if (irq_table[irq].installed)
        return UACPI_STATUS_ALREADY_EXISTS;

    irq_table[irq].handler = handler;
    irq_table[irq].ctx = ctx;
    irq_table[irq].installed = true;

    idt_set_gate(irq + 32, (uint64_t) isr_trampolines[irq], 0x08, 0x8E);

    if (out_irq_handle)
        *out_irq_handle = (uacpi_handle) (uintptr_t) irq;

    return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,
                                         uacpi_handle irq_handle) {

    uint32_t irq = (uint32_t) (uintptr_t) irq_handle;
    if (irq >= IDT_ENTRIES || !irq_table[irq].installed)
        return UACPI_STATUS_NOT_FOUND;

    if (irq_table[irq].handler != handler)
        return UACPI_STATUS_INVALID_ARGUMENT;

    irq_table[irq].installed = false;
    irq_table[irq].handler = NULL;
    irq_table[irq].ctx = NULL;

    idt_set_gate(irq + 32, 0, 0, 0);

    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_spinlock(void) {

    struct spinlock *lock = kzalloc(sizeof(struct spinlock));
    return lock;
}

void uacpi_kernel_free_spinlock(uacpi_handle a) {

    kfree(a);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle a) {

    bool flag = spin_lock((struct spinlock *) a);
    return flag;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle a, uacpi_cpu_flags b) {
    spin_unlock((struct spinlock *) a, b);
}
uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return (uacpi_thread_id) 0;
}

//
//
//
// stuff down here is unfinished/not complete
// vvvvvvvvvvv
//
//
//
//
//

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type, uacpi_work_handler,
                                        uacpi_handle) {

    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {

    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_mutex(void) {

    return kzalloc(8);
}
void uacpi_kernel_free_mutex(uacpi_handle a) {

    kfree(a);
}
uacpi_handle uacpi_kernel_create_event(void) {

    return kzalloc(8);
}
void uacpi_kernel_free_event(uacpi_handle a) {

    kfree(a);
}
uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle, uacpi_u16) {
    return UACPI_STATUS_OK;
}
void uacpi_kernel_release_mutex(uacpi_handle) {}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle, uacpi_u16) {
    return false;
}
void uacpi_kernel_signal_event(uacpi_handle) {}

void uacpi_kernel_reset_event(uacpi_handle) {}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *) {

    return UACPI_STATUS_UNIMPLEMENTED;
}
