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

/* TODO: file is very messy. clean this up you goober >:C */

static bool wait_until_clear(uint32_t *reg, uint32_t mask) {
    uint64_t timeout_ms = AHCI_CMD_TIMEOUT_MS;
    while ((mmio_read_32(reg) & mask) && timeout_ms--) {
        sleep_ms(1);
    }
    return (mmio_read_32(reg) & mask) == 0;
}

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
    struct ahci_cmd_header **hdr =
        kzalloc(sizeof(struct ahci_cmd_header *) * 32);

    if (!arr || !hdr)
        k_panic("Could not allocate space for AHCI commands\n");

    struct ahci_full_port p = {.port = port,
                               .fis = fis,
                               .cmd_list_base = cmdlist,
                               .cmd_tables = arr,
                               .cmd_hdrs = hdr};
    dev->regs[port_num] = p;
}

static struct ahci_disk *device_setup(struct ahci_device *dev,
                                      struct ahci_controller *ctrl,
                                      uint32_t *disk_count) {
    uint32_t pi = mmio_read_32(&ctrl->pi);

    uint32_t total_disks = 0;

    // Only check ports that are implemented
    for (uint32_t i = 0; i < 32; i++) {
        if (!(pi & (1U << i)))
            continue;

        struct ahci_port *port = ahci_get_port(dev, i);

        mmio_write_32(&port->cmd, mmio_read_32(&port->cmd) & ~AHCI_CMD_ST);
        wait_until_clear(&port->cmd, AHCI_CMD_CR);

        mmio_write_32(&port->cmd, mmio_read_32(&port->cmd) & ~AHCI_CMD_FRE);
        wait_until_clear(&port->cmd, AHCI_CMD_FR);

        mmio_write_32(&port->is, 0xFFFFFFFF);

        uint32_t cmd = mmio_read_32(&port->cmd);
        cmd |= AHCI_CMD_FRE | AHCI_CMD_ST;
        mmio_write_32(&port->cmd, cmd);

        uint32_t ssts = mmio_read_32(&port->ssts);
        uint32_t det = ssts & 0x0F;
        uint32_t ipm = (ssts >> 8) & 0x0F;
        if (!(det == AHCI_DET_PRESENT && ipm == AHCI_IPM_ACTIVE))
            continue;

        uint32_t sig = mmio_read_32(&port->sig);
        ahci_info(K_INFO, "Controller port %u has signature 0x%lx", i, sig);

        if (sig == 0xFFFFFFFF)
            continue;

        if (port->sig != 0x00000101) {
            ahci_info(K_WARN, "Controller port %u is not an HDD, skipping...",
                      i, sig);
            continue;
        }

        total_disks++;
    }

    if (!total_disks)
        return NULL;

    *disk_count = total_disks;

    struct ahci_disk *disks = kzalloc(sizeof(struct ahci_disk) * total_disks);
    if (!disks)
        k_panic("Could not allocate space for AHCI disks\n");

    uint32_t disks_ind = 0;

    for (uint32_t i = 0; i < 32; i++) {
        if (!(pi & (1U << i)))
            continue;

        struct ahci_port *port = ahci_get_port(dev, i);
        uint32_t ssts = mmio_read_32(&port->ssts);

        if ((ssts & 0x0F) == AHCI_DET_PRESENT &&
            ((ssts >> 8) & 0x0F) == AHCI_IPM_ACTIVE) {
            disks[disks_ind].port = i;
            disks[disks_ind].device = dev;

            uint32_t cmd = mmio_read_32(&port->cmd);
            mmio_write_32(&port->cmd, cmd & ~(AHCI_CMD_ST | AHCI_CMD_FRE));

            uint64_t timeout = AHCI_CMD_TIMEOUT_MS;
            while (mmio_read_32(&port->cmd) & (AHCI_CMD_CR | AHCI_CMD_FR)) {
                sleep_ms(1);
                timeout--;
                if (timeout == 0)
                    return NULL;
            }

            allocate_port(dev, port, i);

            cmd = mmio_read_32(&port->cmd);
            cmd |= AHCI_CMD_FRE;
            cmd |= AHCI_CMD_ST;
            mmio_write_32(&port->cmd, cmd);

            setup_port_slots(dev, i);
            ahci_info(K_INFO, "Port %u slots set up", i);
        }
    }
    return disks;
}

struct ahci_disk *ahci_setup_controller(struct ahci_controller *ctrl,
                                        uint32_t *d_cnt) {
    bool s64a = mmio_read_32(&ctrl->cap) & (1U << 31);
    if (!s64a) {
        ahci_info(K_WARN, "controller does not support 64-bit addressing\n");
        return NULL;
    }

    mmio_write_32(&ctrl->ghc, AHCI_GHC_HR);

    uint64_t timeout = AHCI_CMD_TIMEOUT_MS;
    while (mmio_read_32(&ctrl->ghc) & AHCI_GHC_HR) {
        sleep_ms(1);
        if (--timeout == 0) {
            ahci_info(K_WARN, "controller reset timed out\n");
            return NULL;
        }
    }

    mmio_write_32(&ctrl->ghc, mmio_read_32(&ctrl->ghc) | AHCI_GHC_AE);

    struct ahci_device *dev = kzalloc(sizeof(struct ahci_device));
    dev->ctrl = ctrl;
    uint32_t disk_count = 0;
    struct ahci_disk *d = device_setup(dev, ctrl, &disk_count);
    *d_cnt = disk_count;
    ahci_info(K_INFO, "Device initialized successfully, %u port(s) present",
              disk_count);
    return d;
}

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

    void *abar_virt = vmm_map_phys(abar_base, map_size);
    if (!abar_virt) {
        ahci_info(K_ERROR, "failed to map BAR - likely OOM error");
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
    if (!d)
        ahci_info(K_ERROR, "could not allocate space for device");

    d->driver_data = disk;
    d->sector_size = 512;
    d->read_sector = ahci_read_sector_wrapper;
    d->write_sector = ahci_write_sector_wrapper;
    d->print = ahci_print_wrapper;
    d->cache = kmalloc(sizeof(struct block_cache));
    block_cache_init(d->cache, DEFAULT_BLOCK_CACHE_SIZE);
    d->type = G_AHCI_DRIVE;
    return d;
}
