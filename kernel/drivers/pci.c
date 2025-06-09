#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/registry.h>
#include <drivers/ahci.h>
#include <drivers/nvme.h>
#include <drivers/usb.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <pci/pci.h>

static struct pci_device *pci_devices = NULL;
static uint64_t pci_device_count;

const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
    case 0x01:
        switch (subclass) {
        case 0x00: return "SCSI Controller";
        case 0x01: return "IDE Controller";
        case 0x02: return "Floppy Controller";
        case 0x03: return "IPI Bus Controller";
        case 0x04: return "RAID Controller";
        case 0x05: return "ATA Controller";
        case 0x06: return "SATA Controller (AHCI)";
        case 0x08: return "NVMe Controller";
        default: return "Other Mass Storage Controller";
        }
    case 0x02: return "Network Controller";
    case 0x03: return "Display Controller";
    case 0x04: return "Multimedia Controller";
    case 0x06: return "Bridge Device";
    case 0x0C:
        if (subclass == 0x03)
            return "USB Controller";
        break;
    }
    return "Unknown Device";
}

void pci_scan_devices(struct pci_device **devices_out, uint64_t *count_out) {
    pci_device_count = 0;
    uint64_t space_to_alloc = 0;

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            for (uint8_t function = 0; function < 8; ++function) {
                if (pci_read_word(bus, device, function, 0x00) != 0xFFFF)
                    space_to_alloc++;

                if (function == 0) {
                    uint8_t header_type =
                        pci_read_byte(bus, device, function, 0x0E);
                    if ((header_type & 0x80) == 0)
                        break;
                }
            }
        }
    }

    pci_devices = kmalloc(space_to_alloc * sizeof(struct pci_device));

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            for (uint8_t function = 0; function < 8; ++function) {
                uint16_t vendor_id = pci_read_word(bus, device, function, 0x00);
                if (vendor_id == 0xFFFF)
                    continue;

                uint16_t device_id = pci_read_word(bus, device, function, 0x02);
                uint32_t class_info = pci_read(bus, device, function, 0x08);
                uint8_t class_code = (class_info >> 24) & 0xFF;
                uint8_t subclass = (class_info >> 16) & 0xFF;
                uint8_t prog_if = (class_info >> 8) & 0xFF;
                uint8_t revision = class_info & 0xFF;

                pci_devices[pci_device_count++] =
                    (struct pci_device) {.bus = bus,
                                         .device = device,
                                         .function = function,
                                         .vendor_id = vendor_id,
                                         .device_id = device_id,
                                         .class_code = class_code,
                                         .subclass = subclass,
                                         .prog_if = prog_if,
                                         .revision = revision};

                if (class_code == 0x0C && subclass == 0x03) {
                    const char *controller_type = "Unknown";
                    switch (prog_if) {
                    case 0x00: controller_type = "UHCI (USB 1.1)"; break;
                    case 0x10: controller_type = "OHCI (USB 1.1)"; break;
                    case 0x20: controller_type = "EHCI (USB 2.0)"; break;
                    case 0x30: controller_type = "XHCI (USB 3.0+)"; break;
                    }

                    k_printf("Found USB controller: %s at %02x:%02x.%x\n",
                             controller_type, bus, device, function);
                    switch (prog_if) {
                    case 0x30: usb_init(bus, device, function);
                    case 0x00:
                    case 0x10:
                    case 0x20: break;
                    }
                }

                if (function == 0) {
                    uint8_t header_type =
                        pci_read_byte(bus, device, function, 0x0E);
                    if ((header_type & 0x80) == 0)
                        break;
                }
            }
        }
    }

    *devices_out = pci_devices;
    *count_out = pci_device_count;
}

uint32_t pci_read_bar(uint8_t bus, uint8_t device, uint8_t function,
                      uint8_t bar_index) {
    uint8_t offset = 0x10 + (bar_index * 4);
    return pci_read(bus, device, function, offset);
}
