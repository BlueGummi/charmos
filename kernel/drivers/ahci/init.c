#include <asm.h>
#include <console/printf.h>
#include <drivers/ahci.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// TODO: Hand this off to IDE if the GHC bit 31 is OFF
// It won't be AHCI - Sometimes we are in IDE emul mode

/* TODO: file is very messy. clean this up you goober >:C */

static void setup_port_slots(struct ahci_device *dev, uint32_t port_id) {
    struct ahci_full_port *port = &dev->regs[port_id];
    for (uint64_t slot = 0; slot < 32; slot++) {
        uint64_t cmdtbl_phys = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);

        void *cmdtbl_virt = vmm_map_phys(cmdtbl_phys, PAGE_SIZE,
                                         PAGING_UNCACHABLE, VMM_FLAG_NONE);
        memset(cmdtbl_virt, 0, PAGE_SIZE);

        struct ahci_cmd_header *cmd_header =
            (port->cmd_list_base + slot * sizeof(struct ahci_cmd_header));

        cmd_header->ctba = (uint32_t) (cmdtbl_phys & 0xFFFFFFFF);
        cmd_header->ctbau = (uint32_t) (cmdtbl_phys >> 32);
        cmd_header->prdtl = 1;
        port->cmd_tables[slot] = cmdtbl_virt;
        port->cmd_hdrs[slot] = cmd_header;
    }
}

static void allocate_port(struct ahci_device *dev, struct ahci_port *port,
                          uint32_t port_num) {
    uint64_t cmdlist_phys = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    uint64_t fis_phys = pmm_alloc_page(ALLOC_FLAGS_DEFAULT);
    void *cmdlist =
        vmm_map_phys(cmdlist_phys, PAGE_SIZE, PAGING_UNCACHABLE, VMM_FLAG_NONE);
    void *fis =
        vmm_map_phys(fis_phys, PAGE_SIZE, PAGING_UNCACHABLE, VMM_FLAG_NONE);
    memset(cmdlist, 0, PAGE_SIZE);
    memset(fis, 0, PAGE_SIZE);

    port->clb = cmdlist_phys & 0xFFFFFFFFUL;
    port->clbu = cmdlist_phys >> 32;
    port->fb = fis_phys & 0xFFFFFFFFUL;
    port->fbu = fis_phys >> 32;
    struct ahci_cmd_table **arr =
        kzalloc(sizeof(struct ahci_cmd_table *) * 32, ALLOC_PARAMS_DEFAULT);
    struct ahci_cmd_header **hdr =
        kzalloc(sizeof(struct ahci_cmd_header *) * 32, ALLOC_PARAMS_DEFAULT);

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

    mmio_write_32(&dev->ctrl->ghc, 1 << 1);
    uint32_t total_disks = 0;

    for (uint32_t i = 0; i < 32; i++) {
        if (!(pi & (1U << i)))
            continue;

        struct ahci_port *port = ahci_get_port(dev, i);

        mmio_write_32(&port->cmd, mmio_read_32(&port->cmd) & ~AHCI_CMD_ST);
        mmio_wait(&port->cmd, AHCI_CMD_CR, AHCI_CMD_TIMEOUT_MS);

        mmio_write_32(&port->cmd, mmio_read_32(&port->cmd) & ~AHCI_CMD_FRE);
        mmio_wait(&port->cmd, AHCI_CMD_FR, AHCI_CMD_TIMEOUT_MS);

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

        if (sig != 0x00000101) {
            ahci_info(K_WARN, "Controller port %u is not an HDD, skipping...",
                      i, sig);
            continue;
        }

        dev->port_count++;
        total_disks++;
    }

    if (!total_disks)
        return NULL;

    *disk_count = total_disks;

    struct ahci_disk *disks =
        kzalloc(sizeof(struct ahci_disk) * total_disks, ALLOC_PARAMS_DEFAULT);
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
            uint32_t sig = mmio_read_32(&port->sig);

            if (sig != 0x00000101)
                continue;

            disks[disks_ind].port = i;
            disks[disks_ind].device = dev;

            uint32_t cmd = mmio_read_32(&port->cmd);
            mmio_write_32(&port->is, 0xFFFFFFFF);
            mmio_write_32(&port->ie, 0xFFFFFFFF);
            mmio_write_32(&port->cmd, cmd & ~(AHCI_CMD_ST | AHCI_CMD_FRE));

            mmio_wait(&port->cmd, AHCI_CMD_CR | AHCI_CMD_FR,
                      AHCI_CMD_TIMEOUT_MS);

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

    mmio_wait(&ctrl->ghc, AHCI_GHC_HR, AHCI_CMD_TIMEOUT_MS);

    mmio_write_32(&ctrl->ghc, mmio_read_32(&ctrl->ghc) | AHCI_GHC_AE);

    struct ahci_device *dev =
        kzalloc(sizeof(struct ahci_device), ALLOC_PARAMS_DEFAULT);
    if (!dev)
        k_panic("Could not allocate space for AHCI device setup\n");

    dev->ctrl = ctrl;
    dev->irq_num = irq_alloc_entry();

    uint32_t disk_count = 0;
    struct ahci_disk *d = device_setup(dev, ctrl, &disk_count);
    *d_cnt = disk_count;
    ahci_info(K_INFO,
              "Device initialized successfully, %u usable port(s) present",
              disk_count);

    return d;
}
