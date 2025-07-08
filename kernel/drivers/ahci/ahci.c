#include <acpi/ioapic.h>
#include <drivers/ahci.h>
#include <drivers/pci.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <registry.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asm.h"
#include "block/bcache.h"
#include "block/generic.h"
#include "console/printf.h"

struct ahci_disk *ahci_discover_device(uint8_t bus, uint8_t device,
                                       uint8_t function,
                                       uint32_t *out_disk_count) {
    ahci_info(K_INFO, "Found device at %02x:%02x.%x", bus, device, function);
    uint32_t abar = pci_read(bus, device, function, 0x24);
    uint32_t abar_base = abar & ~0xFU;

    pci_write(bus, device, function, 0x24, 0xFFFFFFFF);
    uint32_t size_mask = pci_read(bus, device, function, 0x24);
    pci_write(bus, device, function, 0x24, abar);

    if (size_mask == 0 || size_mask == 0xFFFFFFFF) {
        ahci_info(K_WARN, "invalid BAR size");
        return NULL;
    }

    uint64_t abar_size = ~(size_mask & ~0xFU) + 1;
    uint64_t map_size = (abar_size + 0xFFF) & ~0xFFFU;

    void *abar_virt = vmm_map_phys(abar_base, map_size, PAGING_UNCACHABLE);
    if (!abar_virt) {
        ahci_info(K_ERROR, "failed to map BAR - likely OOM error");
        return NULL;
    }
    uint8_t irq_line = pci_read(bus, device, function, 0x3C);

    ahci_info(K_INFO, "AHCI device uses IRQ %u ", irq_line);

    struct ahci_controller *ctrl = (struct ahci_controller *) abar_virt;

    struct ahci_disk *disk = ahci_setup_controller(ctrl, out_disk_count);
    if (!disk)
        return NULL;

    isr_register(disk->device->irq_num, ahci_isr_handler, disk->device, 0);
    ioapic_route_irq(irq_line, disk->device->irq_num, get_sch_core_id(), false);
    return disk;
}

void ahci_print_wrapper(struct generic_disk *d) {
    struct ahci_disk *a = d->driver_data;
    ahci_identify(a);
}

struct generic_disk *ahci_create_generic(struct ahci_disk *disk) {
    struct generic_disk *d = kmalloc(sizeof(struct generic_disk));
    if (!d)
        ahci_info(K_ERROR, "could not allocate space for device");

    d->driver_data = disk;
    d->sector_size = 512;
    d->read_sector = ahci_read_sector_wrapper;
    d->write_sector = ahci_write_sector_wrapper;
    d->submit_bio_async = ahci_submit_bio_request;
    d->print = ahci_print_wrapper;
    d->cache = kzalloc(sizeof(struct bcache));
    bcache_init(d->cache, DEFAULT_BLOCK_CACHE_SIZE);
    d->type = G_AHCI_DRIVE;
    return d;
}

static uint64_t ahci_cnt = 1;

static void ahci_pci_init(uint8_t bus, uint8_t slot, uint8_t func,
                          struct pci_device *dev) {
    (void) dev;
    uint32_t d_cnt = 0;
    struct ahci_disk *disks = ahci_discover_device(bus, slot, func, &d_cnt);
    for (uint32_t i = 0; i < d_cnt; i++) {
        struct generic_disk *disk = ahci_create_generic(&disks[i]);
        registry_mkname(disk, "sata", ahci_cnt++);
        registry_register(disk);
        k_print_register(disk->name);
    }
}

REGISTER_PCI_DEV(ahci, 1, 6, 1, 0xFFFF, ahci_pci_init)
