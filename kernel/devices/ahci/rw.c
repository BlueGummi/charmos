#include <asm.h>
#include <console/printf.h>
#include <devices/ahci.h>
#include <devices/ata.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>

static void ahci_set_lba_cmd(struct ahci_fis_reg_h2d *fis, uint64_t lba,
                             uint16_t sector_count) {
    fis->device = 1 << 6;

    fis->lba0 = (uint8_t) (lba & 0xFF);
    fis->lba1 = (uint8_t) ((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t) ((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t) ((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t) ((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t) ((lba >> 40) & 0xFF);

    fis->countl = (uint8_t) (sector_count & 0xFF);
    fis->counth = (uint8_t) ((sector_count >> 8) & 0xFF);
}

bool ahci_read_sector(struct generic_disk *disk, uint64_t lba,
                      uint8_t *out_buf) {
    struct ahci_disk *ahci_disk = (struct ahci_disk *) disk->driver_data;
    struct ahci_full_port *port = &ahci_disk->device->regs[ahci_disk->port];

    uint32_t slot = find_free_cmd_slot(port->port);
    if (slot == (uint32_t) -1)
        return false;

    uint64_t buffer_phys;
    uint8_t *buffer = ahci_prepare_command(port, slot, false, &buffer_phys);
    if (!buffer)
        return false;

    struct ahci_cmd_table *cmd_tbl = port->cmd_tables[slot];
    ahci_setup_fis(cmd_tbl, AHCI_CMD_READ_DMA_EXT, false);
    ahci_set_lba_cmd((struct ahci_fis_reg_h2d *) cmd_tbl->cfis, lba, 1);

    bool ok = ahci_send_command(port, slot);
    if (ok)
        memcpy(out_buf, buffer, 512);

    return ok;
}

bool ahci_write_sector(struct generic_disk *disk, uint64_t lba,
                       const uint8_t *in_buf) {
    struct ahci_disk *ahci_disk = (struct ahci_disk *) disk->driver_data;
    struct ahci_full_port *port = &ahci_disk->device->regs[ahci_disk->port];

    uint32_t slot = find_free_cmd_slot(port->port);
    if (slot == (uint32_t) -1)
        return false;

    uint64_t buffer_phys;
    uint8_t *buffer = ahci_prepare_command(port, slot, true, &buffer_phys);
    if (!buffer)
        return false;

    memcpy(buffer, in_buf, 512);

    struct ahci_cmd_table *cmd_tbl = port->cmd_tables[slot];
    ahci_setup_fis(cmd_tbl, AHCI_CMD_WRITE_DMA_EXT, false);
    ahci_set_lba_cmd((struct ahci_fis_reg_h2d *) cmd_tbl->cfis, lba, 1);

    return ahci_send_command(port, slot);
}
