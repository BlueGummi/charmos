#include <acpi/print.h>
#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <compiler/core.h>
#include <console/printf.h>
#include <drivers/mmio.h>
#include <irq/idt.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <pit.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/mutex.h>
#include <sync/mutex_simple.h>
#include <sync/spinlock.h>
#include <thread/thread.h>
#include <uacpi/event.h>
#include <uacpi/platform/arch_helpers.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>

#include "uacpi/kernel_api.h"
#include <types/freq.h>
#include <types/types.h>

#include "uacpi/log.h"
#include "uacpi/namespace.h"

freq_hz_t tsc_freq = 0;

#define panic_if_error(x)                                                      \
    if (uacpi_unlikely_error(x))                                               \
        panic("uACPI initialization failed!");

static uint64_t our_rsdp = 0;
void uacpi_init(uint64_t rsdp) {
    our_rsdp = rsdp;
    tsc_freq = pit_measure_tsc_freq();

    panic_if_error(uacpi_initialize(0));
    panic_if_error(uacpi_namespace_load());
    panic_if_error(uacpi_namespace_initialize());
    panic_if_error(uacpi_finalize_gpe_initialization());
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {

    if (our_rsdp == 0) {
        printf("no rsdp set\n");
        return UACPI_STATUS_INTERNAL_ERROR;
    }
    *out_rsdp_address = our_rsdp;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    void *ret = (void *) mmio_map(addr, len);
    return ret;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    return;
    vmm_unmap(addr, len);
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *data) {
    switch (level) {
    case UACPI_LOG_ERROR: printf(">> UACPI ERROR: %s", data); break;
    case UACPI_LOG_TRACE: printf(">> UACPI TRACE: %s", data); break;
    case UACPI_LOG_INFO: printf(">> UACPI INFO: %s", data); break;
    case UACPI_LOG_WARN: printf(">> UACPI WARN: %s", data); break;
    default: break;
    }
}

void *uacpi_kernel_alloc(uacpi_size size) {
    void *ret = valloc(size);
    return ret;
}

void uacpi_kernel_free(void *mem) {
    vfree(mem);
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len,
                                 uacpi_handle *out_handle) {
    if (!out_handle || len == 0) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uacpi_io_handle *handle = vnew(handle);
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
    vfree(handle);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    if (tsc_freq == 0)
        return 0;

    return (rdtsc() * 1000000000ull) / tsc_freq;
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    uint64_t start = rdtsc();
    uint64_t target = start + (HZ_TO_MHZ(tsc_freq) * usec);

    while (rdtsc() < target)
        cpu_pause();
}

void uacpi_kernel_sleep(uacpi_u64 msec) {

    for (uacpi_u64 i = 0; i < msec * 10; i++)
        uacpi_kernel_stall(100);
}

struct uacpi_irq_shim {
    uacpi_interrupt_handler handler;
    uacpi_handle ctx;
};

static struct uacpi_irq_shim uacpi_irq_shims[256];

static enum irq_result uacpi_irq_trampoline(struct irq_context *ictx,
                                            void *data) {
    cc_ignore(ictx);
    struct uacpi_irq_shim *shim = data;
    return shim->handler(shim->ctx) == UACPI_INTERRUPT_HANDLED ? IRQ_HANDLED
                                                               : IRQ_NONE;
}

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle) {
    irq += 32;

    if (irq >= 256)
        return UACPI_STATUS_INVALID_ARGUMENT;

    uacpi_irq_shims[irq].handler = handler;
    uacpi_irq_shims[irq].ctx = ctx;
    irq_register("uacpi", irq, uacpi_irq_trampoline, &uacpi_irq_shims[irq],
                 IRQ_FLAG_NONE);

    if (out_irq_handle)
        *out_irq_handle = (uacpi_handle) (uintptr_t) irq;

    return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,
                                         uacpi_handle irq_handle) {
    uint32_t irq = (uint32_t) (uintptr_t) irq_handle;

    cc_ignore(handler);

    if (irq >= 256)
        return UACPI_STATUS_INVALID_ARGUMENT;

    irq_free_entry(irq);
    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_spinlock(void) {
    struct spinlock *lock = vnew(lock, ALLOC_ZERO);
    spinlock_init(lock);
    return lock;
}

void uacpi_kernel_free_spinlock(uacpi_handle a) {
    vfree(a);
}

uacpi_handle uacpi_kernel_create_mutex(void) {
    struct mutex_simple *m = vnew(m, ALLOC_ZERO);
    mutex_simple_init(m);
    return m;
}

void uacpi_kernel_free_mutex(uacpi_handle a) {
    vfree(a);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle m,
                                        uacpi_u16 b) TSA_NO_ANALYSIS {
    cc_ignore(b);
    mutex_simple_lock(m);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_release_mutex(uacpi_handle m) TSA_NO_ANALYSIS {
    mutex_simple_unlock(m);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle a) TSA_NO_ANALYSIS {
    return spin_lock((struct spinlock *) a);
}

void uacpi_kernel_unlock_spinlock(uacpi_handle a,
                                  uacpi_cpu_flags b) TSA_NO_ANALYSIS {
    spin_unlock((struct spinlock *) a, b);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return thread_get_current();
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

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,
                                        uacpi_work_handler handler,
                                        uacpi_handle ctx) {
    cc_ignore(type, handler, ctx);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {

    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_event(void) {

    return valloc(8, ALLOC_ZERO);
}
void uacpi_kernel_free_event(uacpi_handle a) {

    vfree(a);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    cc_ignore(handle, timeout);
    return false;
}
void uacpi_kernel_signal_event(uacpi_handle handle) {
    cc_ignore(handle);
}

void uacpi_kernel_reset_event(uacpi_handle handle) {
    cc_ignore(handle);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req) {
    cc_ignore(req);
    return UACPI_STATUS_UNIMPLEMENTED;
}
