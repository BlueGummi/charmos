#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <uacpi/kernel_api.h>

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address,
                                          uacpi_handle *out_handle) {

    if (!out_handle) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    if (address.segment != 0) {
        k_printf("PCI segment %u not supported\n", address.segment);
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uint8_t bus = address.bus;
    uint8_t slot = address.device;
    uint8_t func = address.function;

    uacpi_pci_device *dev = kzalloc(sizeof(*dev));

    if (!dev) {
        return UACPI_STATUS_OUT_OF_MEMORY;
    }

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
    if (!dev || !value || !dev->is_open || offset >= 256) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *value = pci_read_byte(dev->bus, dev->slot, dev->func, offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset,
                                     uacpi_u16 *value) {

    uacpi_pci_device *dev = (uacpi_pci_device *) device;
    if (!dev || !value || !dev->is_open || offset >= 256 || (offset & 1)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    *value = pci_read_word(dev->bus, dev->slot, dev->func, offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset,
                                     uacpi_u32 *value) {

    uacpi_pci_device *dev = (uacpi_pci_device *) device;
    if (!dev || !value || !dev->is_open || offset >= 256 || (offset & 3)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    *value = pci_read(dev->bus, dev->slot, dev->func, offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset,
                                     uacpi_u8 value) {

    uacpi_pci_device *dev = (uacpi_pci_device *) device;
    if (!dev || !dev->is_open || offset >= 256) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    pci_write_byte(dev->bus, dev->slot, dev->func, offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset,
                                      uacpi_u16 value) {

    uacpi_pci_device *dev = (uacpi_pci_device *) device;
    if (!dev || !dev->is_open || offset >= 256 || (offset & 1)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    pci_write_word(dev->bus, dev->slot, dev->func, offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset,
                                      uacpi_u32 value) {

    uacpi_pci_device *dev = (uacpi_pci_device *) device;
    if (!dev || !dev->is_open || offset >= 256 || (offset & 3)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    pci_write(dev->bus, dev->slot, dev->func, offset, value);
    return UACPI_STATUS_OK;
}

#define IO_ACCESS_GUARD()                                                      \
    if (!handle || !handle->valid || offset >= handle->len ||                  \
        (handle->base + offset) > 0xFFFF) {                                    \
        return UACPI_STATUS_INVALID_ARGUMENT;                                  \
    }

uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size offset,
                                   uacpi_u8 *out) {
    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    IO_ACCESS_GUARD();
    *out = inb((uint16_t) (handle->base + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size offset,
                                    uacpi_u16 *out) {
    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    IO_ACCESS_GUARD();
    *out = inw((uint16_t) (handle->base + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size offset,
                                    uacpi_u32 *out) {
    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    IO_ACCESS_GUARD();
    *out = inl((uint16_t) (handle->base + offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size offset,
                                    uacpi_u8 val) {
    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    IO_ACCESS_GUARD();
    outb((uint16_t) (handle->base + offset), val);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size offset,
                                     uacpi_u16 val) {
    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    IO_ACCESS_GUARD();
    outw((uint16_t) (handle->base + offset), val);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size offset,
                                     uacpi_u32 val) {
    uacpi_io_handle *handle = (uacpi_io_handle *) h;
    IO_ACCESS_GUARD();
    outl((uint16_t) (handle->base + offset), val);
    return UACPI_STATUS_OK;
}
