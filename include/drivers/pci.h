/* @title: PCI */
#pragma once
#include <compiler.h>
#include <asm.h>
#include <stdint.h>

#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_NVM 0x08
#define PCI_PROGIF_NVME 0x02

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

struct pci_driver {
    char *name;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    void (*initialize)(uint8_t, uint8_t, uint8_t, struct pci_device *);
} __linker_aligned; 

extern struct pci_driver __skernel_pci_devices[];
extern struct pci_driver __ekernel_pci_devices[];

#define PCI_DEV_REGISTER(n, cc, sc, pi, vi, init)                              \
    static struct pci_driver pci_device_##n __attribute__((                    \
        section(".kernel_pci_devices"), used)) = {.name = #n,                  \
                                                  .class_code = cc,            \
                                                  .subclass = sc,              \
                                                  .prog_if = pi,               \
                                                  .vendor_id = vi,             \
                                                  .initialize = init};

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
} __packed;

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
    uint32_t address = (1U << 31) // enable bit
                       | ((uint32_t) bus << 16) | ((uint32_t) device << 11) |
                       ((uint32_t) function << 8) |
                       (offset & 0xFC); // aligned to 4 bytes
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t data = inl(PCI_CONFIG_DATA);

    if (offset & 2)
        return (uint16_t) (data >> 16);
    else
        return (uint16_t) (data & 0xFFFF);
}

static inline uint8_t pci_read_config8(uint8_t bus, uint8_t device,
                                       uint8_t function, uint8_t offset) {
    uint32_t address = (1U << 31) | ((uint32_t) bus << 16) |
                       ((uint32_t) device << 11) | ((uint32_t) function << 8) |
                       (offset & 0xFC); // 4-byte aligned
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t data = inl(PCI_CONFIG_DATA);

    return (uint8_t) ((data >> ((offset & 3) * 8)) & 0xFF);
}

static inline void pci_write_config16(uint8_t bus, uint8_t device,
                                      uint8_t function, uint8_t offset,
                                      uint16_t value) {
    uint32_t address = (1U << 31) | ((uint32_t) bus << 16) |
                       ((uint32_t) device << 11) | ((uint32_t) function << 8) |
                       (offset & 0xFC);
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
    return (uint32_t) ((1U << 31) | (bus << 16) | (slot << 11) | (func << 8) |
                       (offset & 0xFC));
}

static inline uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func,
                                uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static inline uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func,
                                     uint8_t offset) {
    uint32_t value = pci_read(bus, slot, func, offset & 0xFC);
    return (value >> ((offset & 2) * 8)) & 0xFFFF;
}

static inline uint8_t pci_read_byte(uint8_t bus, uint8_t slot, uint8_t func,
                                    uint8_t offset) {
    uint32_t value = pci_read(bus, slot, func, offset & 0xFC);
    return (value >> ((offset & 3) * 8)) & 0xFF;
}

static inline void pci_write(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

static inline void pci_write_word(uint8_t bus, uint8_t slot, uint8_t func,
                                  uint8_t offset, uint16_t value) {
    uint32_t tmp = pci_read(bus, slot, func, offset & 0xFCU);
    uint32_t shift = (offset & 2) * 8;
    tmp = (tmp & ~(0xFFFFU << shift)) | ((uint32_t) value << shift);
    pci_write(bus, slot, func, offset & 0xFCU, tmp);
}

static inline void pci_write_byte(uint8_t bus, uint8_t slot, uint8_t func,
                                  uint8_t offset, uint8_t value) {
    uint32_t tmp = pci_read(bus, slot, func, offset & 0xFC);
    uint32_t shift = (offset & 3) * 8;
    tmp = (tmp & ~(0xFF << shift)) | ((uint32_t) value << shift);
    pci_write(bus, slot, func, offset & 0xFC, tmp);
}

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
