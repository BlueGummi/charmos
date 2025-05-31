#include <mem/alloc.h>
#include <disk/generic_disk.h>
#include <asm.h>
#include <pci/pci.h>
#include <console/printf.h>

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

    for (uint8_t bus = 0; bus < 255; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            for (uint8_t function = 0; function < 8; ++function) {
                if (pci_read_word(bus, device, function, 0x0) != 0xFFFF)
                    space_to_alloc++;
            }
        }
    }

    pci_devices = kmalloc(space_to_alloc);

    for (uint8_t bus = 0; bus < 255; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            for (uint8_t function = 0; function < 8; ++function) {
                uint16_t vendor_id = pci_read_word(bus, device, function, 0x00);
                if (vendor_id == 0xFFFF)
                    continue;

                uint16_t device_id = pci_read_word(bus, device, function, 0x02);
                uint8_t class_code = pci_read_byte(bus, device, function, 0x0B);
                uint8_t subclass = pci_read_byte(bus, device, function, 0x0A);
                uint8_t prog_if = pci_read_byte(bus, device, function, 0x09);
                uint8_t revision = pci_read_byte(bus, device, function, 0x08);

                if (pci_device_count < MAX_PCI_DEVICES) {
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
