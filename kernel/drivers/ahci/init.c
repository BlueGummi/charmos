#include <asm.h>
#include <console/printf.h>
#include <drivers/ahci.h>
#include <drivers/ata.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <s_assert.h>
#include <sleep.h>
#include <string.h>

// TODO: Hand this off to IDE if the GHC bit 31 is OFF
// It won't be AHCI - Sometimes we are in IDE emul mode

static void setup_port_slots(struct ahci_device *dev, uint32_t port_id) {
    struct ahci_full_port *port = &dev->regs[port_id];
    for (int slot = 0; slot < 32; slot++) {
        uint64_t cmdtbl_phys = (uint64_t) pmm_alloc_page(false);
        void *cmdtbl_virt = vmm_map_phys(cmdtbl_phys, 4096);
        memset(cmdtbl_virt, 0, 4096);

        struct ahci_cmd_header *cmd_header =
            (port->cmd_list_base +
             (uint64_t) slot * sizeof(struct ahci_cmd_header));

        cmd_header->ctba = (uint32_t) (cmdtbl_phys & 0xFFFFFFFF);
        cmd_header->ctbau = (uint32_t) (cmdtbl_phys >> 32);
        cmd_header->prdtl = 1;
        port->cmd_tables[slot] = cmdtbl_virt;
        port->cmd_hdrs[slot] = cmd_header;
    }
}

static void allocate_port(struct ahci_device *dev, struct ahci_port *port,
                          uint32_t port_num) {
    uint64_t cmdlist_phys = (uint64_t) pmm_alloc_page(false);
    uint64_t fis_phys = (uint64_t) pmm_alloc_page(false);
    void *cmdlist = vmm_map_phys(cmdlist_phys, 4096);
    void *fis = vmm_map_phys(fis_phys, 4096);
    memset(cmdlist, 0, 4096);
    memset(fis, 0, 4096);

    port->clb = cmdlist_phys & 0xFFFFFFFFUL;
    port->clbu = cmdlist_phys >> 32;
    port->fb = fis_phys & 0xFFFFFFFFUL;
    port->fbu = fis_phys >> 32;
    struct ahci_cmd_table **arr = kzalloc(sizeof(struct ahci_cmd_table *) * 32);

    struct ahci_cmd_header **hdrr =
        kzalloc(sizeof(struct ahci_cmd_header *) * 32);

    struct ahci_full_port p = {.port = port,
                               .fis = fis,
                               .cmd_list_base = cmdlist,
                               .cmd_tables = arr,
                               .cmd_hdrs = hdrr};
    dev->regs[port_num] = p;
}

static struct ahci_disk *device_setup(struct ahci_device *dev,
                                      struct ahci_controller *ctrl,
                                      uint32_t *disk_count) {
    uint32_t pi = ctrl->pi;

    uint32_t total_disks = 0;

    for (uint32_t i = 0; i < 32; i++) {
        uint32_t ssts = ctrl->ports[i].ssts;
        if (pi & (1U << i) && (ssts & 0x0F) == AHCI_DET_PRESENT &&
            ((ssts >> 8) & 0x0F) == AHCI_IPM_ACTIVE) {
            total_disks += 1;
        }
    }

    if (!total_disks)
        return NULL;
    *disk_count = total_disks;

    struct ahci_disk *disks = kzalloc(sizeof(struct ahci_disk) * total_disks);
    uint32_t disks_ind = 0;

    for (uint32_t i = 0; i < 32; i++) {
        uint32_t ssts = ctrl->ports[i].ssts;
        if (pi & (1U << i) && (ssts & 0x0F) == AHCI_DET_PRESENT &&
            ((ssts >> 8) & 0x0F) == AHCI_IPM_ACTIVE) {
            disks[disks_ind].port = i;
            disks[disks_ind].device = dev;
            struct ahci_port *port = &ctrl->ports[i];
            port->cmd &= ~AHCI_CMD_ST;
            port->cmd &= ~AHCI_CMD_FRE;

            allocate_port(dev, port, i);

            port->cmd |= AHCI_CMD_FRE;
            port->cmd |= AHCI_CMD_ST;

            while (port->cmd & AHCI_CMD_CR)
                ;

            setup_port_slots(dev, i);
        }
    }
    return disks;
}

// TODO: big time reorganization

struct ahci_disk *ahci_setup_controller(struct ahci_controller *ctrl,
                                        uint32_t *d_cnt) {
    bool s64a = ctrl->cap & (1U << 31);
    if (!s64a) {
        k_printf("AHCI controller does not support 64-bit addressing\n");
        return NULL;
    }

    while ((ctrl->ghc & AHCI_GHC_HR) != 0)
        ;
    ctrl->ghc |= AHCI_GHC_AE;

    struct ahci_device *dev = kzalloc(sizeof(struct ahci_device));
    uint32_t disk_count = 0;
    struct ahci_disk *d = device_setup(dev, ctrl, &disk_count);
    *d_cnt = disk_count;
    return d;
}

struct ahci_disk *ahci_discover_device(uint8_t bus, uint8_t device,
                                       uint8_t function,
                                       uint32_t *out_disk_count) {
    uint32_t abar = pci_read(bus, device, function, 0x24);
    uint32_t abar_base = abar & ~0xFU;

    pci_write(bus, device, function, 0x24, 0xFFFFFFFF);
    uint32_t size_mask = pci_read(bus, device, function, 0x24);
    pci_write(bus, device, function, 0x24, abar);

    if (size_mask == 0 || size_mask == 0xFFFFFFFF) {
        k_printf("Invalid AHCI BAR size at %02x:%02x.%x\n", bus, device,
                 function);
        return NULL;
    }

    uint64_t abar_size = ~(size_mask & ~0xFU) + 1;
    uint64_t map_size = (abar_size + 0xFFF) & ~0xFFFU;

    void *abar_virt = vmm_map_phys(abar_base, map_size);
    if (!abar_virt) {
        k_printf("Failed to map AHCI BAR at %08x (size %zu)\n", abar_base,
                 map_size);
        return NULL;
    }
    struct ahci_controller *ctrl = (struct ahci_controller *) abar_virt;
    return ahci_setup_controller(ctrl, out_disk_count);
}

void ahci_print_wrapper(struct generic_disk *d) {
    struct ahci_disk *a = d->driver_data;
    ahci_identify(a);
}

struct generic_disk *ahci_create_generic(struct ahci_disk *disk) {
    struct generic_disk *d = kmalloc(sizeof(struct generic_disk));
    d->driver_data = disk;
    d->sector_size = 512;
    d->read_sector = ahci_read_sector_wrapper;
    d->write_sector = ahci_write_sector_wrapper;
    d->print = ahci_print_wrapper;
    d->type = G_AHCI_DRIVE;
    return d;
}
