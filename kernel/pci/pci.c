#include <io.h>
#include <printf.h>

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

void scan_pci_devices() {
    for (uint8_t bus = 0; bus < 255; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            for (uint8_t function = 0; function < 8; ++function) {
                uint16_t vendor_id = pci_read_word(bus, device, function, 0x00);
                if (vendor_id == 0xFFFF)
                    continue; // no device

                uint16_t device_id = pci_read_word(bus, device, function, 0x02);
                uint8_t class_code = pci_read_byte(bus, device, function, 0x0B);
                uint8_t subclass = pci_read_byte(bus, device, function, 0x0A);
                uint8_t prog_if = pci_read_byte(bus, device, function, 0x09);
                uint8_t revision = pci_read_byte(bus, device, function, 0x08);

                const char *type = pci_class_name(class_code, subclass);

                k_printf("PCI Device: bus %u, device %u, function %u\n", bus,
                         device, function);
                k_printf("  Vendor ID: 0x%04x, Device ID: 0x%04x\n", vendor_id,
                         device_id);
                k_printf("  Class: 0x%02x (%s), Subclass: 0x%02x, ProgIF: "
                         "0x%02x, Revision: 0x%02x\n\n",
                         class_code, type, subclass, prog_if, revision);

                if (function == 0) {
                    uint8_t header_type =
                        pci_read_byte(bus, device, function, 0x0E);
                    if ((header_type & 0x80) == 0)
                        break;
                }
            }
        }
    }
}
