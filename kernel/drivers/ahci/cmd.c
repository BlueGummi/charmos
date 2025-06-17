#include <asm.h>
#include <console/printf.h>
#include <drivers/ahci.h>
#include <drivers/ata.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <string.h>

#define MAX_PRDT_ENTRY_SIZE (4 * 1024 * 1024) // 4MB

uint32_t find_free_cmd_slot(struct ahci_port *port) {
    uint32_t slots_in_use = mmio_read_32(&port->sact) | mmio_read_32(&port->ci);

    for (int slot = 0; slot < 32; slot++) {
        if ((slots_in_use & (1U << slot)) == 0) {
            return slot;
        }
    }

    return -1;
}

void *ahci_prepare_command(struct ahci_full_port *port, uint32_t slot,
                           bool write, uint64_t *out_phys, uint64_t size) {
    struct ahci_cmd_header *hdr = port->cmd_hdrs[slot];
    struct ahci_cmd_table *cmd_tbl = port->cmd_tables[slot];

    if (!hdr || !cmd_tbl || size == 0)
        return NULL;

    uint64_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t prdt_count =
        (size + MAX_PRDT_ENTRY_SIZE - 1) / MAX_PRDT_ENTRY_SIZE;

    if (prdt_count > 65535)
        return NULL;

    uint64_t phys_base = (uint64_t) pmm_alloc_pages(page_count, false);
    if (!phys_base)
        return NULL;

    void *virt_base = vmm_map_phys(phys_base, page_count * PAGE_SIZE);

    if (out_phys)
        *out_phys = phys_base;

    hdr->cfl = sizeof(struct ahci_fis_reg_h2d) / sizeof(uint32_t);
    hdr->w = write ? 1 : 0;
    hdr->p = 0;
    hdr->a = 0;
    hdr->c = 1;
    hdr->prdtl = prdt_count;
    hdr->prdbc = 0;

    uint64_t remaining = size;
    uint64_t offset = 0;
    for (uint32_t i = 0; i < prdt_count; i++) {
        uint64_t chunk =
            (remaining > MAX_PRDT_ENTRY_SIZE) ? MAX_PRDT_ENTRY_SIZE : remaining;

        uint64_t phys_addr = phys_base + offset;

        cmd_tbl->prdt_entry[i].dba = (uint32_t) (phys_addr & 0xFFFFFFFF);
        cmd_tbl->prdt_entry[i].dbau = (uint32_t) (phys_addr >> 32);
        cmd_tbl->prdt_entry[i].dbc = (uint32_t) (chunk - 1); // size - 1
        cmd_tbl->prdt_entry[i].i = (i == prdt_count - 1) ? 1 : 0;

        offset += chunk;
        remaining -= chunk;
    }

    return virt_base;
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
    mmio_write_32(&port->port->is, (uint32_t) -1);

    uint32_t ci = mmio_read_32(&port->port->ci);
    ci |= (1 << slot);
    mmio_write_32(&port->port->ci, ci);

    uint64_t timeout = AHCI_CMD_TIMEOUT_MS;
    while (mmio_read_32(&port->port->ci) & (1 << slot)) {
        sleep_ms(1);
        timeout--;
        if (timeout == 0)
            return false;
    }

    timeout = AHCI_CMD_TIMEOUT_MS;
    while (mmio_read_32(&port->port->tfd) & (STATUS_BSY & STATUS_DRQ) &&
           --timeout) {
        sleep_ms(1);
        if (timeout == 0)
            return false;
    }

    uint32_t tfd = mmio_read_32(&port->port->tfd);
    if (tfd & (1 << 0) || tfd & (1 << 1)) {
        return false;
    }

    return true;
}

void ahci_identify(struct ahci_disk *disk) {
    struct ahci_full_port *port = &disk->device->regs[disk->port];
    uint32_t slot = find_free_cmd_slot(port->port);

    uint64_t buffer_phys;
    void *buffer = ahci_prepare_command(port, slot, false, &buffer_phys, 4096);

    if (!buffer)
        return;

    ahci_setup_fis(port->cmd_tables[slot], AHCI_CMD_IDENTIFY, false);

    if (ahci_send_command(port, slot)) {
        struct ata_identify *ident = (struct ata_identify *) buffer;
        ata_ident_print(ident);
    } else {
        k_printf("AHCI IDENTIFY failed\n");
    }
}
