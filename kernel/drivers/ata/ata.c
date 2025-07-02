#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/ahci.h>
#include <drivers/ata.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <drivers/pci.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

void ata_select_drive(struct ata_drive *ata_drive) {
    uint16_t base = ata_drive->io_base;

    outb(REG_DRIVE_HEAD(base), 0xA0 | (ata_drive->slave ? 0x10 : 0x00));
    io_wait();
}

void ata_soft_reset(struct ata_drive *ata_drive) {
    uint16_t ctrl = ata_drive->ctrl_base;

    outb(ctrl, 0x04); // nIEN=0, SRST=1
    io_wait();

    outb(ctrl, 0x00); // nIEN=0, SRST=0
    io_wait();

    uint16_t base = ata_drive->io_base;
    uint64_t timeout = IDE_CMD_TIMEOUT_MS * 1000;

    while (inb(REG_STATUS(base)) & STATUS_BSY) {
        sleep_us(10);
        timeout--;
        if (timeout == 0)
            return;
    }
}

bool ata_identify(struct ata_drive *ata_drive) {
    ata_select_drive(ata_drive);
    io_wait();

    outb(REG_COMMAND(ata_drive->io_base), AHCI_CMD_IDENTIFY);
    uint8_t status = inb(REG_STATUS(ata_drive->io_base));

    if (status == 0)
        return false;

    uint64_t timeout = IDE_IDENT_TIMEOUT_MS * 1000;
    while ((status & STATUS_BSY)) {
        status = inb(REG_STATUS(ata_drive->io_base));
        sleep_us(1);
        timeout--;
        if (timeout == 0)
            return false;
    }

    if (inb(REG_LBA_MID(ata_drive->io_base)) ||
        inb(REG_LBA_HIGH(ata_drive->io_base)))
        return false;

    insw(ata_drive->io_base, (uint16_t *) ata_drive->identify_data, 256);
    return true;
}

bool ata_setup_drive(struct ata_drive *ide, struct pci_device *devices,
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
            if (!ide->identify_data)
                k_panic("Could not allocate space for IDE Identify\n");

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
