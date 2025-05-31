#include <ahci.h>
#include <asm.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <string.h>

// TODO: BIG TIME MACRO DEFS HERE!!!! HOLY MOLY THIS IS SO BAD!!!

static void *ahci_map_registers(uintptr_t abar) {
    size_t size = 0x400;

    uintptr_t phys =
        (uintptr_t) pmm_alloc_pages((size + 0xFFF) / 0x1000, false);
    if (!phys)
        return NULL;

    void *virt = (void *) ((uintptr_t) vmm_map_phys(phys, size) & ~0xFFF);
    if (!virt) {
        pmm_free_pages((void *) phys, (size + 0xFFF) / 0x1000, false);
        return NULL;
    }

    memcpy(virt, (void *) abar, size);

    return virt;
}

static bool ahci_port_wait_ready(struct ahci_port *port) {
    if (port->cmd & AHCI_CMD_CR) {
        port->cmd &= ~AHCI_CMD_ST;

        uint64_t timeout = 500;
        while ((port->cmd & AHCI_CMD_CR) && timeout--) {
            sleep(1);
        }
        if (timeout <= 0)
            return false;
    }

    uint64_t timeout = 500;
    while (port->cmd & AHCI_CMD_CR) {
        sleep(1);
        if (timeout-- <= 0)
            return false;
    }

    return true;
}

bool ahci_port_init(struct ahci_port *port) {
    if (ahci_port_wait_ready(port))
        return false;

    void *cl = kmalloc(1024);
    void *fis = kmalloc(256);
    if (!cl || !fis) {
        kfree(cl);
        kfree(fis);
        return false;
    }

    memset(cl, 0, 1024);
    memset(fis, 0, 256);

    uintptr_t cl_phys = vmm_get_phys((uintptr_t) cl);
    uintptr_t fis_phys = vmm_get_phys((uintptr_t) fis);

    port->clb = cl_phys & 0xFFFFFFFF;
    port->clbu = (cl_phys >> 32) & 0xFFFFFFFF;
    port->fb = fis_phys & 0xFFFFFFFF;
    port->fbu = (fis_phys >> 32) & 0xFFFFFFFF;

    port->serr = 0xFFFFFFFF;

    port->cmd |= AHCI_CMD_FRE;
    port->cmd |= AHCI_CMD_SUD;

    uint64_t timeout = 500;
    while (!(port->ssts & 0xF) && timeout--) {
        sleep(1);
    }
    if (timeout <= 0)
        return false;

    port->cmd |= AHCI_CMD_ST;

    timeout = 500;
    while (!(port->cmd & AHCI_CMD_FR) && timeout--) {
        sleep(1);
    }
    if (timeout <= 0)
        return false;

    return true;
}

bool ahci_execute_command(struct ahci_device *dev, uint8_t command,
                          void *buffer, size_t size, uint64_t lba) {
    struct ahci_port *port = dev->regs;

    if (ahci_port_wait_ready(port))
        return false;

    struct ahci_cmd_header *cmd_list =
        (struct ahci_cmd_header *) vmm_get_phys(port->clb);
    struct ahci_cmd_table *cmd_table = kmalloc(256);
    if (!cmd_table)
        return false;
    memset(cmd_table, 0, 256);

    cmd_list[0].flags = (5 << 0); // 5 DWORDs = 20 bytes CFIS
    if (command == 0x35) {        // write
        cmd_list[0].flags |= AHCI_CMD_FLAGS_WRITE;
    }
    cmd_list[0].prdtl = AHCI_CMD_FLAGS_PRDTL;
    uintptr_t ctba_phys = vmm_get_phys((uintptr_t) cmd_table);
    cmd_list[0].ctba = ctba_phys & 0xFFFFFFFF;
    cmd_list[0].ctbau = (ctba_phys >> 32) & 0xFFFFFFFF;

    uint8_t *fis = cmd_table->cfis;
    fis[0] = FIS_TYPE_REG_H2D;
    fis[1] = FIS_REG_CMD;
    fis[2] = command;
    fis[3] = 0;
    fis[4] = lba & 0xFF;
    fis[5] = (lba >> 8) & 0xFF;
    fis[6] = (lba >> 16) & 0xFF;
    fis[7] = LBA_MODE | ((lba >> 24) & 0x0F);
    fis[8] = (lba >> 32) & 0xFF;
    fis[9] = (lba >> 40) & 0xFF;
    fis[10] = (size / 512) & 0xFF;
    fis[11] = ((size / 512) >> 8) & 0xFF;
    fis[15] = CONTROL_BIT;

    struct ahci_prdt_entry *prdt = cmd_table->prdt;
    prdt[0].dba = vmm_get_phys((uintptr_t) buffer);
    prdt[0].dbc = (size - 1) & 0x3FFFFF;

    port->ci = 1 << 0;

    int timeout = 500;
    while ((port->ci & (1 << 0)) && timeout--) {
        sleep(1);
    }

    if (timeout <= 0 || (port->is & AHCI_DEV_BUSY)) {
        kfree(cmd_table);
        return false;
    }

    kfree(cmd_table);
    return true;
}

void ahci_print_ctrlr(struct ahci_controller *ctrl) {
    uint32_t version_major = (ctrl->vs >> 16) & 0xFFFF;
    uint32_t version_minor = ctrl->vs & 0xFFFF;
    uint32_t cap_np = (ctrl->cap >> 8) & 0x1F;

    k_printf("AHCI Controller Information:\n");
    k_printf("  Version: %d.%d\n", version_major, version_minor);
    k_printf("  Ports Implemented: 0x%08X\n", ctrl->pi);
    k_printf("  Number of Ports: %d\n", cap_np + 1);
    k_printf("  Capabilities: 0x%08X\n", ctrl->cap);
}

void ahci_pci_discover() {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t class = pci_read(bus, slot, func, 0x08);
                uint8_t class_code = (class >> 24) & 0xFF;
                uint8_t subclass = (class >> 16) & 0xFF;
                uint8_t prog_if = (class >> 8) & 0xFF;

                if (class_code == 0x01 && subclass == 0x06 && prog_if == 0x01) {
                    uint32_t abar = pci_read(bus, slot, func, 0x24) & ~0xF;
                    void *abar_virt = vmm_map_phys(abar, 0x1100);

                    struct ahci_controller *ctrl =
                        (struct ahci_controller *) abar_virt;
                    ahci_print_ctrlr(ctrl);
                }
            }
        }
    }
}
