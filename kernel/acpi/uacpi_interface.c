#include <acpi/print.h>
#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <pit.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/mutex.h>
#include <sync/spin_lock.h>
#include <uacpi/event.h>
#include <uacpi/platform/arch_helpers.h>
#include <uacpi/status.h>
#include <uacpi/uacpi.h>

#include "uacpi/kernel_api.h"
#include "uacpi/log.h"
#include "uacpi/namespace.h"
#include "uacpi/platform/types.h"
#include "uacpi/types.h"

uint64_t tsc_freq = 0;

#define panic_if_error(x)                                                      \
    if (uacpi_unlikely_error(x))                                               \
        k_panic("uACPI initialization failed!\n");

void uacpi_init() {
    tsc_freq = measure_tsc_freq_pit();

    panic_if_error(uacpi_initialize(0));
    panic_if_error(uacpi_namespace_load());
    panic_if_error(uacpi_namespace_initialize());
    panic_if_error(uacpi_finalize_gpe_initialization());
}

void uacpi_print_devs() {
    uacpi_namespace_for_each_child(uacpi_namespace_root(), acpi_print_ctx,
                                   UACPI_NULL, UACPI_OBJECT_DEVICE_BIT,
                                   UACPI_MAX_DEPTH_ANY, UACPI_NULL);
}

extern uint64_t a_rsdp;

extern uint64_t tsc_freq;

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
    return vmm_map_phys(addr, len, PAGING_UNCACHABLE);
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
        return UACPI_STATUS_INTERNAL_ERROR;
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
        cpu_relax();
}

void uacpi_kernel_sleep(uacpi_u64 msec) {

    for (uacpi_u64 i = 0; i < msec * 10; ++i)
        uacpi_kernel_stall(100);
}

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle) {
    irq += 32;

    if (irq >= 256)
        return UACPI_STATUS_INVALID_ARGUMENT;

    isr_register(irq, (void *) handler, ctx, get_sch_core_id());

    if (out_irq_handle)
        *out_irq_handle = (uacpi_handle) (uintptr_t) irq;

    return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,
                                         uacpi_handle irq_handle) {
    uint32_t irq = (uint32_t) (uintptr_t) irq_handle;
    irq += 32;

    (void) handler;

    if (irq >= 256)
        return UACPI_STATUS_INVALID_ARGUMENT;

    if (!idt_is_installed(irq))
        return UACPI_STATUS_NOT_FOUND;

    idt_free_entry(irq);
    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_spinlock(void) {

    struct spinlock *lock = kzalloc(sizeof(struct spinlock));
    return lock;
}

void uacpi_kernel_free_spinlock(uacpi_handle a) {

    kfree(a);
}

uacpi_handle uacpi_kernel_create_mutex(void) {
    struct mutex *m = kzalloc(sizeof(struct mutex));
    return m;
}

void uacpi_kernel_free_mutex(uacpi_handle a) {
    kfree(a);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle m, uacpi_u16 b) {
    (void) b;
    mutex_lock(m);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_release_mutex(uacpi_handle m) {
    mutex_unlock(m);
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

uacpi_handle uacpi_kernel_create_event(void) {

    return kzalloc(8);
}
void uacpi_kernel_free_event(uacpi_handle a) {

    kfree(a);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle, uacpi_u16) {
    return false;
}
void uacpi_kernel_signal_event(uacpi_handle) {}

void uacpi_kernel_reset_event(uacpi_handle) {}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *) {

    return UACPI_STATUS_UNIMPLEMENTED;
}
