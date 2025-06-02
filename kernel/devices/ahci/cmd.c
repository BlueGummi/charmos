#include <asm.h>
#include <console/printf.h>
#include <devices/ahci.h>
#include <devices/ata.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>

uint32_t find_free_cmd_slot(struct ahci_port *port) {
    uint32_t slots_in_use = port->sact | port->ci;
    for (int slot = 0; slot < 32; slot++) {
        if ((slots_in_use & (1U << slot)) == 0) {
            return slot;
        }
    }

    return -1;
}

void *ahci_prepare_command(struct ahci_full_port *port, uint32_t slot,
                           bool write, uint64_t *out_phys) {
    struct ahci_cmd_header *hdr = port->cmd_hdrs[slot];
    struct ahci_cmd_table *cmd_tbl = port->cmd_tables[slot];

    uint64_t buffer_phys = (uint64_t) pmm_alloc_page(false);
    void *buffer = vmm_map_phys(buffer_phys, 4096);
    if (out_phys)
        *out_phys = buffer_phys;

    hdr->cfl = sizeof(struct ahci_fis_reg_h2d) / sizeof(uint32_t);
    hdr->w = write ? 1 : 0;
    hdr->p = 0;
    hdr->a = 0;
    hdr->c = 1;
    hdr->prdbc = 0;

    cmd_tbl->prdt_entry[0].dba = (uint32_t) (buffer_phys & 0xFFFFFFFF);
    cmd_tbl->prdt_entry[0].dbau = (uint32_t) (buffer_phys >> 32);
    cmd_tbl->prdt_entry[0].dbc = 511;
    cmd_tbl->prdt_entry[0].i = 1;

    return buffer;
}

void ahci_setup_fis(struct ahci_cmd_table *cmd_tbl, uint8_t command,
                    bool is_atapi) {
    struct ahci_fis_reg_h2d *fis = (struct ahci_fis_reg_h2d *) cmd_tbl->cfis;
    memset(fis, 0, sizeof(struct ahci_fis_reg_h2d));

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = command;

    if (is_atapi) {
        fis->device = 1 << 6; // LBA bit
    }
}

bool ahci_send_command(struct ahci_full_port *port, uint32_t slot) {
    port->port->is = (uint32_t) -1;

    port->port->ci |= (1 << slot);

    while (port->port->ci & (1 << slot)) {}

    if (port->port->tfd & (1 << 0) || port->port->tfd & (1 << 1)) {
        return false;
    }

    return true;
}

void ahci_identify(struct ahci_disk *disk) {
    struct ahci_full_port *port = &disk->device->regs[disk->port];
    uint32_t slot = find_free_cmd_slot(port->port);

    uint64_t buffer_phys;
    void *buffer = ahci_prepare_command(port, slot, false, &buffer_phys);
    ahci_setup_fis(port->cmd_tables[slot], AHCI_CMD_IDENTIFY, false);

    if (ahci_send_command(port, slot)) {
        struct ata_identify *ident = (struct ata_identify *) buffer;
        ata_ident_print(ident);
    } else {
        k_printf("AHCI IDENTIFY failed\n");
    }
}
