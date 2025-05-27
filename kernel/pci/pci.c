#include <disk.h>
#include <io.h>
#include <pci.h>
#include <printf.h>

static struct pci_device pci_devices[MAX_PCI_DEVICES];
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

void scan_pci_devices(struct pci_device **devices_out, uint64_t *count_out) {
    pci_device_count = 0;

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

void setup_primary_ide(struct ide_drive *ide, struct pci_device *devices,
                       uint64_t count) {
    ide->sector_size = 512;
    for (uint64_t i = 0; i < count; i++) {
        struct pci_device *curr = &devices[i];

        if (curr->class_code == 1 && curr->subclass == 1) {
            uint32_t bar0 =
                pci_read_bar(curr->bus, curr->device, curr->function, 0);
            uint32_t bar1 =
                pci_read_bar(curr->bus, curr->device, curr->function, 1);

            if ((bar0 & 1) == 1) {
                ide->io_base = (uint16_t) (bar0 & 0xFFFFFFFC);
            } else {
                ide->io_base = 0x1F0;
            }

            if ((bar1 & 1) == 1) {
                ide->ctrl_base = (uint16_t) (bar1 & 0xFFFFFFFC);
            } else {
                ide->ctrl_base = 0x3F6;
            }

            ide->slave = 0;
            return;
        }
    }

    ide->io_base = 0x1F0;
    ide->ctrl_base = 0x3F6;
    ide->slave = 0;
}
