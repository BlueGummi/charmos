#include <alloc.h>
#include <idt.h>
#include <io.h>
#include <pmm.h>
#include <printf.h>
#include <slab.h>
#include <spin_lock.h>
#include <stdint.h>
#include <uacpi/event.h>
#include <uacpi/internal/types.h>
#include <uacpi/platform/arch_helpers.h>
#include <uacpi/status.h>
#include <uacpi/uacpi.h>
#include <vmm.h>

typedef struct { // typedefing it because of consistency with uacpi naming
    uint8_t bus, slot, func;
    bool is_open;
} uacpi_pci_device;

typedef struct {
    uacpi_io_addr base;
    uacpi_size len;
    bool valid;
} uacpi_io_handle;

typedef struct {
    uacpi_interrupt_handler handler;
    uacpi_handle ctx;
    bool installed;
} irq_entry_t;
static irq_entry_t irq_table[IDT_ENTRIES];

extern uint64_t a_rsdp;

static uacpi_io_handle global_io_handle;
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

static uintptr_t uacpi_map_top = 0;

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    uintptr_t phys_start = PAGE_ALIGN_DOWN(addr);
    uintptr_t offset = addr - phys_start;

    size_t total_len = len + offset;
    size_t total_pages = (total_len + PAGE_SIZE - 1) / PAGE_SIZE;

    if (uacpi_map_top + total_pages * PAGE_SIZE > UACPI_MAP_LIMIT) {
        k_panic("uACPI: out of virtual space in uacpi_kernel_map");
        return NULL;
    }

    uintptr_t virt_start = uacpi_map_top;
    uacpi_map_top += total_pages * PAGE_SIZE;

    for (size_t i = 0; i < total_pages; i++) {
        vmm_map_page(virt_start + i * PAGE_SIZE, phys_start + i * PAGE_SIZE,
                     PAGING_PRESENT | PAGING_WRITE);
    }

    return (void *) (virt_start + offset);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    uintptr_t virt_addr = (uintptr_t) addr;
    uintptr_t page_offset = virt_addr & (PAGE_SIZE - 1);
    uintptr_t aligned_virt = PAGE_ALIGN_DOWN(virt_addr);

    size_t total_len = len + page_offset;
    size_t total_pages = (total_len + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t i = 0; i < total_pages; i++) {
        vmm_unmap_page(aligned_virt + i * PAGE_SIZE);
    }
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *data) {
    k_printf(">> UACPI LOG: LEVEL = %d\n", (int) level);
    k_printf(">> %s\n", data);
}

void *uacpi_kernel_alloc(uacpi_size size) {

    void *x = kmalloc(size);
    return x;
}

void uacpi_kernel_free(void *mem) {
    kfree(mem);
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address,
                                          uacpi_handle *out_handle) {

    if (!out_handle)
        return UACPI_STATUS_INVALID_ARGUMENT;

    if (address.segment != 0) {
        k_printf("PCI segment %u not supported\n", address.segment);
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uint8_t bus = address.bus;
    uint8_t slot = address.device;
    uint8_t func = address.function;

    uacpi_pci_device *dev = kzalloc(sizeof(*dev));
    if (!dev)
        return UACPI_STATUS_OUT_OF_MEMORY;

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->is_open = true;

    *out_handle = dev;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) {

    uacpi_pci_device *dev = (uacpi_pci_device *) handle;

    if (!dev->is_open) {
        return;
    }

    dev->is_open = false;
    uacpi_kernel_free(dev);
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset,
                                    uacpi_u8 *value) {

    uacpi_pci_device *dev = (uacpi_pci_device *) device;
    if (!dev || !value || !dev->is_open || offset >= 256)
        return UACPI_STATUS_INVALID_ARGUMENT;
    *value = pci_read_byte(dev->bus, dev->slot, dev->func, offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset,
                                     uacpi_u16 *value) {

    uacpi_pci_device *dev = (uacpi_pci_device *) device;
    if (!dev || !value || !dev->is_open || offset >= 256 || (offset & 1))
        return UACPI_STATUS_INVALID_ARGUMENT;

    *value = pci_read_word(dev->bus, dev->slot, dev->func, offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset,
                                     uacpi_u32 *value) {

    uacpi_pci_device *dev = (uacpi_pci_device *) device;
    if (!dev || !value || !dev->is_open || offset >= 256 || (offset & 3))
        return UACPI_STATUS_INVALID_ARGUMENT;

    *value = pci_read(dev->bus, dev->slot, dev->func, offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset,
                                     uacpi_u8 value) {

    uacpi_pci_device *dev = (uacpi_pci_device *) device;
    if (!dev || !dev->is_open || offset >= 256)
        return UACPI_STATUS_INVALID_ARGUMENT;

    pci_write_byte(dev->bus, dev->slot, dev->func, offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset,
                                      uacpi_u16 value) {

    uacpi_pci_device *dev = (uacpi_pci_device *) device;
    if (!dev || !dev->is_open || offset >= 256 || (offset & 1))
        return UACPI_STATUS_INVALID_ARGUMENT;

    pci_write_word(dev->bus, dev->slot, dev->func, offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset,
                                      uacpi_u32 value) {

    uacpi_pci_device *dev = (uacpi_pci_device *) device;
    if (!dev || !dev->is_open || offset >= 256 || (offset & 3))
        return UACPI_STATUS_INVALID_ARGUMENT;

    pci_write(dev->bus, dev->slot, dev->func, offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len,
                                 uacpi_handle *out_handle) {

    if (!out_handle)
        return UACPI_STATUS_INVALID_ARGUMENT;

    global_io_handle.base = base;
    global_io_handle.len = len;
    global_io_handle.valid = true;

    *out_handle = &global_io_handle;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {

    (void) handle;
    global_io_handle.valid = false;
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size offset,
                                   uacpi_u8 *out) {

    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    if (!handle || !handle->valid || offset >= handle->len)
        return UACPI_STATUS_INVALID_ARGUMENT;

    *out = inb((uint16_t) (handle->base + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size offset,
                                    uacpi_u16 *out) {

    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    if (!handle || !handle->valid || (offset + 1) >= handle->len)
        return UACPI_STATUS_INVALID_ARGUMENT;

    *out = inw((uint16_t) (handle->base + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size offset,
                                    uacpi_u32 *out) {

    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    if (!handle || !handle->valid || (offset + 3) >= handle->len)
        return UACPI_STATUS_INVALID_ARGUMENT;

    *out = inl((uint16_t) (handle->base + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size offset,
                                    uacpi_u8 val) {

    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    if (!handle || !handle->valid || offset >= handle->len)
        return UACPI_STATUS_INVALID_ARGUMENT;

    outb((uint16_t) (handle->base + offset), val);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size offset,
                                     uacpi_u16 val) {

    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    if (!handle || !handle->valid || (offset + 1) >= handle->len)
        return UACPI_STATUS_INVALID_ARGUMENT;

    outw((uint16_t) (handle->base + offset), val);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size offset,
                                     uacpi_u32 val) {

    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    if (!handle || !handle->valid || (offset + 3) >= handle->len)
        return UACPI_STATUS_INVALID_ARGUMENT;

    outl((uint16_t) (handle->base + offset), val);
    return UACPI_STATUS_OK;
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
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

void (*isr_trampolines[])(void *) = {
#define X(n) [n] = irq##n##_entry,
#include "irq_list.h"
#undef X
};

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

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return (uacpi_thread_id) 0;
}

uacpi_handle uacpi_kernel_create_spinlock(void) {

    struct spinlock *lock = kzalloc(sizeof(struct spinlock));
    return lock;
}

void uacpi_kernel_free_spinlock(uacpi_handle a) {

    kfree(a);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle a) {

    spin_lock((struct spinlock *) a);
    return 0;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle a, uacpi_cpu_flags b) {

    (void) b;
    spin_unlock((struct spinlock *) a);
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type, uacpi_work_handler,
                                        uacpi_handle) {

    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {

    return UACPI_STATUS_OK;
}
