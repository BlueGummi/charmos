#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/ahci.h>
#include <drivers/ata.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <pci/pci.h>
#include <stdbool.h>
#include <stdint.h>

// TODO: Massively reorganize this directory and AHCI

static void ata_select_drive(struct ide_drive *ide) {
    uint16_t base = ide->io_base;

    outb(REG_DRIVE_HEAD(base), 0xA0 | (ide->slave ? 0x10 : 0x00));
    io_wait();
}

static void ata_soft_reset(struct ide_drive *ide) {
    uint16_t ctrl = ide->ctrl_base;

    outb(ctrl, 0x04); // nIEN=0, SRST=1
    io_wait();

    outb(ctrl, 0x00); // nIEN=0, SRST=0
    io_wait();

    uint16_t base = ide->io_base;
    while (inb(REG_STATUS(base)) & STATUS_BSY)
        ;
}

static bool ata_identify(struct ide_drive *ide) {
    ata_select_drive(ide);
    io_wait();

    outb(REG_COMMAND(ide->io_base), AHCI_CMD_IDENTIFY);
    uint8_t status = inb(REG_STATUS(ide->io_base));

    if (status == 0)
        return false;

    while ((status & STATUS_BSY))
        status = inb(REG_STATUS(ide->io_base));
    if (inb(REG_LBA_MID(ide->io_base)) || inb(REG_LBA_HIGH(ide->io_base)))
        return false;

    insw(ide->io_base, (uint16_t *) ide->identify_data, 256);
    return true;
}

static bool atapi_identify(struct ide_drive *ide) {
    ata_select_drive(ide);
    io_wait();

    outb(REG_COMMAND(ide->io_base), 0xA1);
    uint8_t status = inb(REG_STATUS(ide->io_base));

    if (status == 0)
        return false;

    while ((status & STATUS_BSY))
        status = inb(REG_STATUS(ide->io_base));
    if (!(inb(REG_LBA_MID(ide->io_base)) == 0x14 &&
          inb(REG_LBA_HIGH(ide->io_base)) == 0xEB))
        return false;

    insw(ide->io_base, (uint16_t *) ide->identify_data, 256);
    return true;
}

bool ide_setup_drive(struct ide_drive *ide, struct pci_device *devices,
                     uint64_t count, int channel, bool is_slave) {

    for (uint64_t i = 0; i < count; i++) {
        struct pci_device *curr = &devices[i];

        if (curr->class_code == 1 && curr->subclass == 1) {
            uint32_t bar = pci_read_bar(curr->bus, curr->device, curr->function,
                                        channel * 2);
            uint32_t ctrl_bar = pci_read_bar(curr->bus, curr->device,
                                             curr->function, channel * 2 + 1);

            ide->io_base = (bar & 1) ? (bar & 0xFFFFFFFC)
                                     : ((channel == 0) ? ATA_PRIMARY_IO
                                                       : ATA_SECONDARY_IO);

            ide->ctrl_base =
                (ctrl_bar & 1)
                    ? (ctrl_bar & 0xFFFFFFFC)
                    : ((channel == 0) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL);

            ide->slave = is_slave;
            ide->identify_data = kmalloc(512);

            ata_select_drive(ide);
            ata_soft_reset(ide);

            if (ata_identify(ide)) {
                ide->type = IDE_TYPE_ATA;
                ide->sector_size = 512;
                return true;
            } else if (atapi_identify(ide)) {
                ide->type = IDE_TYPE_ATAPI;
                ide->sector_size = 2048;
                return true;
            }

            return false;
        }
    }

    ide->io_base = 0;
    ide->ctrl_base = 0;
    ide->slave = 0;
    return false;
}

struct generic_disk *ide_create_generic(struct ide_drive *ide) {
    ide_identify(ide);
    if (!ide->actually_exists)
        return NULL;

    struct generic_disk *d = kmalloc(sizeof(struct generic_disk));
    d->type = G_IDE_DRIVE;
    d->driver_data = ide;
    d->sector_size = ide->sector_size;
    d->read_sector = ide_read_sector_wrapper;
    d->write_sector = ide_write_sector_wrapper;
    d->print = ide_print_info;
    return d;
}
