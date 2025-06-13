#include <stdint.h>

struct ata_drive;
struct pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;

    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
};

union pci_command_reg {
    uint16_t value;
    struct {
        uint16_t io_space : 1;          // Bit 0
        uint16_t memory_space : 1;      // Bit 1
        uint16_t bus_master : 1;        // Bit 2
        uint16_t special_cycles : 1;    // Bit 3
        uint16_t mem_write_inv : 1;     // Bit 4
        uint16_t vga_snoop : 1;         // Bit 5
        uint16_t parity_error : 1;      // Bit 6
        uint16_t reserved0 : 1;         // Bit 7
        uint16_t serr_enable : 1;       // Bit 8
        uint16_t fast_back : 1;         // Bit 9
        uint16_t interrupt_disable : 1; // Bit 10
        uint16_t reserved1 : 5;         // Bits 11–15
    };
};

const char *pci_class_name(uint8_t class_code, uint8_t subclass);

void pci_scan_devices(struct pci_device **devices_out, uint64_t *count_out);
uint32_t pci_read_bar(uint8_t bus, uint8_t device, uint8_t function,
                      uint8_t bar_index);

#pragma once
