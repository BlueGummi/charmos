#include <acpi/ioapic.h>
#include <drivers/ahci.h>
#include <drivers/pci.h>
#include <irq/idt.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <registry.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ahci_disk *ahci_discover_device(uint8_t bus, uint8_t device,
                                       uint8_t function,
                                       uint32_t *out_disk_count) {
    ahci_info(K_INFO, "Found device at %02x:%02x.%x", bus, device, function);
    uint32_t abar = pci_read(bus, device, function, PCI_BAR5);
    uint32_t abar_base = abar & ~0xFU;

    pci_write(bus, device, function, PCI_BAR5, 0xFFFFFFFF);
    uint32_t size_mask = pci_read(bus, device, function, PCI_BAR5);
    pci_write(bus, device, function, PCI_BAR5, abar);

    if (size_mask == 0 || size_mask == 0xFFFFFFFF) {
        ahci_info(K_WARN, "invalid BAR size");
        return NULL;
    }

    uint64_t abar_size = ~(size_mask & ~0xFU) + 1;
    uint64_t map_size = (abar_size + 0xFFFU) & ~0xFFFU;

    void *abar_virt =
        vmm_map_phys(abar_base, map_size, PAGING_UNCACHABLE, VMM_FLAG_NONE);
    if (!abar_virt) {
        ahci_info(K_ERROR, "failed to map BAR - likely OOM error");
        return NULL;
    }
    uint8_t irq_line = pci_read(bus, device, function, PCI_INTERRUPT_LINE);

    ahci_info(K_INFO, "AHCI device uses IRQ %u ", irq_line);

    struct ahci_controller *ctrl = (struct ahci_controller *) abar_virt;

    struct ahci_disk *disk = ahci_setup_controller(ctrl, out_disk_count);
    if (!disk) {
        ahci_info(K_WARN, "AHCI device unsupported");
        return NULL;
    }

    uint64_t core = smp_core_id();
    irq_register("ahci", disk->device->irq_num, ahci_isr_handler, disk->device, IRQ_FLAG_NONE);
    ioapic_route_irq(irq_line, disk->device->irq_num, core, false);
    return disk;
}

void ahci_print_wrapper(struct generic_disk *d) {
    struct ahci_disk *a = d->driver_data;
    ahci_identify(a);
}

static struct bio_scheduler_ops ahci_sata_ssd_ops = {
    .should_coalesce = noop_should_coalesce,
    .reorder = noop_reorder,
    .do_coalesce = noop_do_coalesce,
    .max_wait_time =
        {
            [BIO_RQ_BACKGROUND] = 30,
            [BIO_RQ_LOW] = 20,
            [BIO_RQ_MEDIUM] = 15,
            [BIO_RQ_HIGH] = 10,
            [BIO_RQ_URGENT] = 0,
        },
    .dispatch_threshold = 96,
    .boost_occupance_limit =
        {
            [BIO_RQ_BACKGROUND] = 50,
            [BIO_RQ_LOW] = 40,
            [BIO_RQ_MEDIUM] = 30,
            [BIO_RQ_HIGH] = 20,
            [BIO_RQ_URGENT] = 8,
        },
    .min_wait_ms = 2,
    .tick_ms = 25,
};

struct generic_disk *ahci_create_generic(struct ahci_disk *disk) {
    struct generic_disk *d =
        kzalloc(sizeof(struct generic_disk), ALLOC_PARAMS_DEFAULT);
    if (!d)
        ahci_info(K_ERROR, "could not allocate space for device");

    ahci_identify(disk);

    d->flags = DISK_FLAG_NO_COALESCE | DISK_FLAG_NO_REORDER;
    d->driver_data = disk;
    d->sector_size = disk->sector_size;
    d->read_sector = ahci_read_sector_wrapper;
    d->write_sector = ahci_write_sector_wrapper;
    d->submit_bio_async = ahci_submit_bio_request;
    d->cache = kzalloc(sizeof(struct bcache), ALLOC_PARAMS_DEFAULT);
    if (!d->cache)
        k_panic("Could not allocate space for AHCI device block cache\n");

    d->scheduler = bio_sched_create(d, &ahci_sata_ssd_ops);
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
