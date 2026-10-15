/* @title: PCI */
#pragma once
#include <asm.h>
#include <compiler/core.h>
#include <device.h>
#include <linker/symbols.h>
#include <log.h>
#include <math/align.h>
#include <math/bit.h>
#include <stdint.h>

#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_NVM 0x08
#define PCI_PROGIF_NVME 0x02

struct pci_device {
    struct device device;
    uint8_t bus;
    uint8_t dev;
    uint8_t function;

    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t class_code;
    enum lock_subclass subclass;
    uint8_t prog_if;
    uint8_t revision;
};

struct pci_driver {
    struct device_driver driver;
    uint8_t class_code;
    enum lock_subclass subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
};

LINKER_SECTION_EXTERN(struct pci_driver, pci_devices);

#define PCI_DEV_REGISTER(n, cc, sc, pi, vi, init)                              \
    LINKER_SECTION_OBJECT(struct pci_driver, pci_devices)                      \
    pci_device_##n = {.driver.name = #n,                                       \
                      .class_code = cc,                                        \
                      .subclass = sc,                                          \
                      .prog_if = pi,                                           \
                      .vendor_id = vi,                                         \
                      .driver.probe = init};

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

struct pci_msix_table_entry {
    uint32_t msg_addr_low;
    uint32_t msg_addr_high;
    uint32_t msg_data;
    uint32_t vector_ctrl; // Bit 0 = Mask
};

struct pci_msix_cap {
    uint8_t cap_id;            // 0x0
    uint8_t next_ptr;          // 0x1
    uint16_t msg_ctl;          // 0x2
    uint32_t table_offset_bir; // 0x4
    uint32_t pba_offset_bir;   // 0x8
} cc_packed;

#define PCI_BAR0 0x10
#define PCI_BAR1 0x14
#define PCI_BAR2 0x18
#define PCI_BAR3 0x1C
#define PCI_BAR4 0x20
#define PCI_BAR5 0x24

#define PCI_CAP_PTR 0x34
#define PCI_CAP_ID_MSIX 0x11
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_INTERRUPT_LINE 0x3C
#define PCI_PROG_IF 0x09

static inline uint16_t pci_read_config16(uint8_t bus, uint8_t device,
                                         uint8_t function, uint8_t offset) {
    uint32_t address = BIT(31) // enable bit
                       | ((uint32_t) bus << 16) | ((uint32_t) device << 11) |
                       ((uint32_t) function << 8) | ALIGN_DOWN(offset, 4);
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t data = inl(PCI_CONFIG_DATA);

    if (offset & 2)
        return (uint16_t) (data >> 16);
    else
        return (uint16_t) (data & 0xFFFF);
}

static inline uint8_t pci_read_config8(uint8_t bus, uint8_t device,
                                       uint8_t function, uint8_t offset) {
    uint32_t address = BIT(31) | ((uint32_t) bus << 16) |
                       ((uint32_t) device << 11) | ((uint32_t) function << 8) |
                       ALIGN_DOWN(offset, 4);
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t data = inl(PCI_CONFIG_DATA);

    return (uint8_t) ((data >> ((offset & 3) * 8)) & 0xFF);
}

static inline void pci_write_config16(uint8_t bus, uint8_t device,
                                      uint8_t function, uint8_t offset,
                                      uint16_t value) {
    uint32_t address = BIT(31) | ((uint32_t) bus << 16) |
                       ((uint32_t) device << 11) | ((uint32_t) function << 8) |
                       ALIGN_DOWN(offset, 4);
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t old_data = inl(PCI_CONFIG_DATA);

    uint32_t new_data;
    if (offset & 2)
        new_data = (old_data & 0x0000FFFF) | ((uint32_t) value << 16);
    else
        new_data = (old_data & 0xFFFF0000) | value;

    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, new_data);
}

static inline uint32_t pci_config_address(uint8_t bus, uint8_t slot,
                                          uint8_t func, uint8_t offset) {
    return (uint32_t) (BIT(31) | (bus << 16) | (slot << 11) | (func << 8) |
                       ALIGN_DOWN(offset, 4));
}

static inline uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func,
                                uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static inline uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func,
                                     uint8_t offset) {
    uint32_t value = pci_read(bus, slot, func, ALIGN_DOWN(offset, 4));
    return (value >> ((offset & 2) * 8)) & 0xFFFF;
}

static inline uint8_t pci_read_byte(uint8_t bus, uint8_t slot, uint8_t func,
                                    uint8_t offset) {
    uint32_t value = pci_read(bus, slot, func, ALIGN_DOWN(offset, 4));
    return (value >> ((offset & 3) * 8)) & 0xFF;
}

static inline void pci_write(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

static inline void pci_write_word(uint8_t bus, uint8_t slot, uint8_t func,
                                  uint8_t offset, uint16_t value) {
    uint32_t tmp = pci_read(bus, slot, func, ALIGN_DOWN(offset, 4));
    uint32_t shift = (offset & 2) * 8;
    tmp = (tmp & ~(0xFFFFU << shift)) | ((uint32_t) value << shift);
    pci_write(bus, slot, func, ALIGN_DOWN(offset, 4), tmp);
}

static inline void pci_write_byte(uint8_t bus, uint8_t slot, uint8_t func,
                                  uint8_t offset, uint8_t value) {
    uint32_t tmp = pci_read(bus, slot, func, ALIGN_DOWN(offset, 4));
    uint32_t shift = (offset & 3) * 8;
    tmp = (tmp & ~(0xFF << shift)) | ((uint32_t) value << shift);
    pci_write(bus, slot, func, ALIGN_DOWN(offset, 4), tmp);
}

LOG_HANDLE_EXTERN(pci);
LOG_SITE_EXTERN(pci);
#define pci_log(log_level, fmt, ...)                                           \
    log(LOG_SITE(pci), LOG_HANDLE(pci), log_level, fmt, ##__VA_ARGS__)

const char *pci_class_name(uint8_t class_code, uint8_t subclass);

void pci_scan_devices(struct pci_device **devices_out, uint64_t *count_out);
uint32_t pci_read_bar(uint8_t bus, uint8_t device, uint8_t function,
                      uint8_t bar_index);
void pci_enable_msix(uint8_t bus, uint8_t slot, uint8_t func);
void pci_enable_msix_on_core(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t vector, uint8_t core);
void pci_init_devices(struct pci_device *devices, uint64_t count);
uint8_t pci_find_capability(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t cap_id);
void pci_program_msix_entry(uint8_t bus, uint8_t slot, uint8_t func,
                            uint32_t table_index, uint8_t vector,
                            uint8_t apic_id);
