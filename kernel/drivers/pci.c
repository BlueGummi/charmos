#include <asm.h>
#include <console/printf.h>
#include <drivers/pci.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <stddef.h>
#include <stdint.h>

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

static void init_device(struct pci_device *dev) {
    struct pci_driver *start = __skernel_pci_devices;
    struct pci_driver *end = __ekernel_pci_devices;

    for (struct pci_driver *d = start; d < end; d++) {
        bool class, subclass, prog_if, vendor;

        class = dev->class_code == d->class_code;
        subclass = dev->subclass == d->subclass;
        prog_if = d->prog_if == 0xFF ? true : dev->prog_if == d->prog_if;
        vendor = d->vendor_id == 0xFFFF ? true : dev->vendor_id == d->vendor_id;

        if (class && subclass && prog_if && vendor) {
            d->initialize(dev->bus, dev->device, dev->function, dev);
        }
    }
}

void pci_init_devices(struct pci_device *devices, uint64_t count) {
    struct pci_driver *start = __skernel_pci_devices;
    struct pci_driver *end = __ekernel_pci_devices;

    k_info("PCI", K_INFO, "There are %u PCI drivers", end - start);

    for (uint64_t i = 0; i < count; i++) {
        struct pci_device *dev = &devices[i];
        init_device(dev);
    }
}

void pci_scan_devices(struct pci_device **devices_out, uint64_t *count_out) {
    pci_device_count = 0;

    uint64_t space_to_alloc = 0;

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            for (uint8_t function = 0; function < 8; ++function) {
                uint16_t vendor = pci_read_word(bus, device, function, 0x00);
                if (vendor == 0xFFFF)
                    continue;

                space_to_alloc++;

                /* TODO: Do not blindly enable all PCI devices */
                union pci_command_reg cmd;
                cmd.value = pci_read_config16(bus, device, function, 0x04);

                cmd.bus_master = 1;
                cmd.memory_space = 1;

                pci_write_config16(bus, device, function, 0x04, cmd.value);

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
    if (!pci_devices)
        k_panic("Could not allocate space for PCI devices\n");

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

                k_info("PCI", K_INFO, "Found device '%s' at %02x:%02x.%x",
                       pci_class_name(class_code, subclass), bus, device,
                       function);

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

uint8_t pci_find_capability(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t cap_id) {
    uint8_t cap_ptr = pci_read_byte(bus, slot, func, PCI_CAP_PTR);

    while (cap_ptr != 0 && cap_ptr != 0xFF) {
        uint8_t current_id = pci_read_byte(bus, slot, func, cap_ptr);
        if (current_id == cap_id) {
            return cap_ptr;
        }
        cap_ptr = pci_read_byte(bus, slot, func, cap_ptr + 1);
    }

    return 0;
}

uint32_t pci_read_bar(uint8_t bus, uint8_t device, uint8_t function,
                      uint8_t bar_index) {
    uint8_t offset = 0x10 + (bar_index * 4);
    return pci_read(bus, device, function, offset);
}

static uint64_t pci_read_bar64(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t bar_index) {
    uint32_t low = pci_read(bus, slot, func, 0x10 + 4 * bar_index);
    uint8_t type = (low >> 1) & 0x3;
    if (type == 0x2) {
        uint32_t high = pci_read(bus, slot, func, 0x10 + 4 * (bar_index + 1));
        return (((uint64_t) high) << 32) | (low & ~0xFULL);
    } else {
        return (uint64_t) (low & ~0xFULL);
    }
}

void pci_program_msix_entry(uint8_t bus, uint8_t slot, uint8_t func,
                            uint32_t table_index, uint8_t vector,
                            uint8_t apic_id) {
    uint8_t cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSIX);
    if (!cap) {
        k_info("PCI", K_ERROR, "MSI-X capability not found");
        return;
    }

    uint32_t table_offset_bir = pci_read(bus, slot, func, cap + 4);
    uint8_t bir = table_offset_bir & 0x7;
    uint32_t table_offset = table_offset_bir & ~0x7;

    if (bir > 5) {
        k_info("PCI", K_ERROR, "MSIX BIR out of range");
        return;
    }

    uint64_t bar_addr = pci_read_bar64(bus, slot, func, bir);
    if (bar_addr == 0) {
        k_info("PCI", K_ERROR, "PCI BAR%u is zero/unassigned", bir);
        return;
    }

    size_t entry_size = sizeof(struct pci_msix_table_entry);
    uint64_t table_base = bar_addr + table_offset; // physical
    uint64_t entry_phys = table_base + (uint64_t) table_index * entry_size;

    uint64_t map_base = entry_phys & ~(PAGE_SIZE - 1);
    size_t map_size = (entry_phys & (PAGE_SIZE - 1)) + entry_size;
    map_size = (map_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    void *map = vmm_map_phys(map_base, map_size, PAGING_UNCACHABLE);
    if (!map) {
        k_info("PCI", K_ERROR, "vmm_map_phys failed for MSI-X table");
        return;
    }

    struct pci_msix_table_entry *entry =
        (struct pci_msix_table_entry *) ((uintptr_t) map +
                                         (entry_phys & (PAGE_SIZE - 1)));

    uint64_t msg_addr = 0xFEE00000ull | ((uint64_t) apic_id << 12);
    mmio_write_32(&entry->msg_addr_low, (uint32_t) (msg_addr & 0xFFFFFFFF));
    mmio_write_32(&entry->msg_addr_high, (uint32_t) (msg_addr >> 32));
    mmio_write_32(&entry->msg_data, (uint32_t) vector);

    uint32_t ctrl = mmio_read_32(&entry->vector_ctrl);
    ctrl &= ~1u;
    mmio_write_32(&entry->vector_ctrl, ctrl);
}

void pci_enable_msix_on_core(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t vector_index, uint8_t apic_id) {
    uint8_t cap = pci_find_capability(bus, slot, func, PCI_CAP_ID_MSIX);
    if (cap == 0) {
        k_info("PCI", K_ERROR, "MSI-X capability not found");
        return;
    }
    uint32_t table_offset_bir = pci_read(bus, slot, func, cap + 4);

    uint8_t bir = table_offset_bir & 0x7;
    uint32_t table_offset = table_offset_bir & ~0x7;

    uint32_t bar_low = pci_read(bus, slot, func, 0x10 + 4 * bir);
    uint32_t bar_high = pci_read(bus, slot, func, 0x10 + 4 * bir + 4);

    uint64_t bar_addr = 0;

    if (bir == 0) {
        bar_addr = ((uint64_t) bar_high << 32) | (bar_low & ~0xFU);
    } else if (bir == 1) {
        k_info("PCI", K_ERROR, "unsupported BIR");
    }

    uint64_t map_size =
        (vector_index + 1) * sizeof(struct pci_msix_table_entry);
    if (map_size < PAGE_SIZE) {
        map_size = PAGE_SIZE;
    }
    void *msix_table =
        vmm_map_phys(bar_addr + table_offset, map_size, PAGING_UNCACHABLE);

    struct pci_msix_table_entry *entry_addr =
        (void *) msix_table +
        vector_index * sizeof(struct pci_msix_table_entry);

    uint64_t msg_addr = 0xFEE00000 | (apic_id << 12);

    mmio_write_32(&entry_addr->msg_addr_low, msg_addr);
    mmio_write_32(&entry_addr->msg_addr_high, 0);
    mmio_write_32(&entry_addr->msg_data, vector_index);

    uint32_t vector_ctrl = mmio_read_32(&entry_addr->vector_ctrl);
    vector_ctrl &= ~0x1;
    mmio_write_32(&entry_addr->vector_ctrl, vector_ctrl);
}

void pci_enable_msix(uint8_t bus, uint8_t slot, uint8_t func) {
    uint8_t cap_ptr = pci_read_byte(bus, slot, func, PCI_CAP_PTR);

    while (cap_ptr != 0) {
        uint8_t cap_id = pci_read_byte(bus, slot, func, cap_ptr);
        if (cap_id == PCI_CAP_ID_MSIX) {
            uint16_t msg_ctl = pci_read_word(bus, slot, func, cap_ptr + 2);

            msg_ctl |= (1 << 15);
            msg_ctl &= ~(1 << 14);

            pci_write_word(bus, slot, func, cap_ptr + 2, msg_ctl);

            uint16_t verify = pci_read_word(bus, slot, func, cap_ptr + 2);

            if ((verify & (1 << 15)) && !(verify & (1 << 14))) {
                k_info("PCI", K_INFO, "MSI-X enabled");
            } else {
                k_info("PCI", K_ERROR, "Failed to enable MSI-X");
            }
            return;
        }
        cap_ptr = pci_read_byte(bus, slot, func, cap_ptr + 1);
    }
    k_info("PCI", K_ERROR, "MSI-X capability not found");
}
