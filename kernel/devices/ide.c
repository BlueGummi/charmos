#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/ide.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <pci/pci.h>
#include <stdbool.h>
#include <stdint.h>

#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376

bool ide_wait_ready(struct ide_drive *d) {
    while (inb(REG_STATUS(d->io_base)) & STATUS_BSY)
        ;
    return (inb(REG_STATUS(d->io_base)) & STATUS_DRDY);
}

bool ide_read_sector(struct ide_drive *d, uint32_t lba, uint8_t *b) {
    if (!ide_wait_ready(d))
        return false;

    outb(REG_DRIVE_HEAD(d->io_base),
         0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F));
    outb(REG_SECTOR_COUNT(d->io_base), 1);
    outb(REG_LBA_LOW(d->io_base), lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (lba >> 16) & 0xFF);
    outb(REG_COMMAND(d->io_base), COMMAND_READ);

    if (!ide_wait_ready(d) || !(inb(REG_STATUS(d->io_base)) & STATUS_DRQ))
        return false;

    insw(REG_DATA(d->io_base), b, 256);
    return true;
}

bool ide_write_sector(struct ide_drive *d, uint32_t lba, const uint8_t *b) {
    if (!ide_wait_ready(d)) {
        return false;
    }

    outb(REG_DRIVE_HEAD(d->io_base), 0xE0 | ((lba >> 24) & 0x0F));

    outb(REG_SECTOR_COUNT(d->io_base), 1);

    outb(REG_LBA_LOW(d->io_base), lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (lba >> 16) & 0xFF);

    outb(REG_COMMAND(d->io_base), COMMAND_WRITE);

    if (!ide_wait_ready(d) || !(inb(REG_STATUS(d->io_base)) & STATUS_DRQ)) {
        return false;
    }

    outsw(REG_DATA(d->io_base), b, 256);

    return true;
}

bool ide_setup_drive(struct ide_drive *ide, struct pci_device *devices,
                     uint64_t count, int channel, int is_slave) {
    ide->sector_size = 512;

    for (uint64_t i = 0; i < count; i++) {
        struct pci_device *curr = &devices[i];

        if (curr->class_code == 1 && curr->subclass == 1) {
            uint32_t bar = pci_read_bar(curr->bus, curr->device, curr->function,
                                        channel * 2);
            uint32_t ctrl_bar = pci_read_bar(curr->bus, curr->device,
                                             curr->function, channel * 2 + 1);

            if ((bar & 1) == 1) {
                ide->io_base = (uint16_t) (bar & 0xFFFFFFFC);
            } else {
                ide->io_base = (channel == 0) ? 0x1F0 : 0x170;
            }

            if ((ctrl_bar & 1) == 1) {
                ide->ctrl_base = (uint16_t) (ctrl_bar & 0xFFFFFFFC);
            } else {
                ide->ctrl_base = (channel == 0) ? 0x3F6 : 0x376;
            }

            ide->slave = is_slave;
            return true;
        }
    }

    ide->io_base = 0;
    ide->ctrl_base = 0;
    ide->slave = 0;
    return false;
}

static void io_wait(void) {
    for (int i = 0; i < 1000; i++)
        inb(0x80);
}

bool ata_identify(uint16_t base, uint16_t ctrl, bool slave) {
    outb(REG_DRIVE_HEAD(base), slave ? 0xB0 : 0xA0);
    io_wait();

    outb(REG_SECTOR_COUNT(base), 0);
    outb(REG_LBA_LOW(base), 0);
    outb(REG_LBA_MID(base), 0);
    outb(REG_LBA_HIGH(base), 0);

    outb(REG_COMMAND(base), ATA_CMD_IDENTIFY);
    io_wait();

    uint8_t status = inb(REG_STATUS(base));
    if (status == 0) {
        return false;
    }

    while (status & STATUS_BSY) {
        status = inb(REG_STATUS(base));
    }

    while (!(status & STATUS_DRQ) && !(status & STATUS_ERR)) {
        status = inb(REG_STATUS(base));
    }

    if (status & STATUS_ERR) {
        return false;
    }

    for (int i = 0; i < 256; i++) {
        inw(REG_DATA(base));
    }

    return true;
}

uint8_t ide_detect_drives(void) {
    const char *channel_names[] = {"Primary", "Secondary"};
    const char *drive_names[] = {"Master", "Slave"};
    uint8_t ret = 0;
    uint16_t io_bases[] = {ATA_PRIMARY_IO, ATA_SECONDARY_IO};
    uint16_t ctrl_bases[] = {ATA_PRIMARY_CTRL, ATA_SECONDARY_CTRL};

    for (int channel = 0; channel < 2; channel++) {
        for (int drive = 0; drive < 2; drive++) {
            int ind = channel * 2 + drive;
            bool found =
                ata_identify(io_bases[channel], ctrl_bases[channel], drive);
            if (found) {
                k_printf("IDE %s %s detected\n", channel_names[channel],
                         drive_names[drive]);
                ret |= (1 << (3 - ind));
            } else {
                k_printf("IDE %s %s not present\n", channel_names[channel],
                         drive_names[drive]);
            }
        }
    }
    return ret;
}

bool ide_read_sector_wrapper(struct generic_disk *d, uint32_t lba,
                             uint8_t *buf) {
    struct ide_drive *ide = d->driver_data;
    return ide_read_sector(ide, lba, buf);
}

bool ide_write_sector_wrapper(struct generic_disk *d, uint32_t lba,
                              const uint8_t *buf) {
    struct ide_drive *ide = d->driver_data;
    return ide_write_sector(ide, lba, buf);
}

struct generic_disk *ide_create_generic(struct ide_drive *ide) {
    struct generic_disk *d = kmalloc(sizeof(struct generic_disk));
    d->driver_data = ide;
    d->sector_size = ide->sector_size;
    d->read_sector = ide_read_sector_wrapper;
    d->write_sector = ide_write_sector_wrapper;
    return d;
}
