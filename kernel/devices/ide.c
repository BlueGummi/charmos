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

bool ide_read_sector(struct ide_drive *d, uint64_t lba, uint8_t *b,
                     uint8_t count) {
    if (count == 0)
        count = 255;

    if (!ide_wait_ready(d))
        return false;

    outb(REG_DRIVE_HEAD(d->io_base),
         0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F));
    outb(REG_SECTOR_COUNT(d->io_base), count);
    outb(REG_LBA_LOW(d->io_base), lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (lba >> 16) & 0xFF);
    outb(REG_COMMAND(d->io_base), COMMAND_READ);

    for (uint8_t i = 0; i < count; i++) {
        if (!ide_wait_ready(d) || !(inb(REG_STATUS(d->io_base)) & STATUS_DRQ))
            return false;

        insw(REG_DATA(d->io_base), b + (i * 512), 256); // 256 words = 512 bytes
    }

    return true;
}

bool ide_write_sector(struct ide_drive *d, uint64_t lba, const uint8_t *b,
                      uint8_t count) {
    if (count == 0)
        count = 255;

    if (!ide_wait_ready(d))
        return false;

    outb(REG_DRIVE_HEAD(d->io_base),
         0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F));
    outb(REG_SECTOR_COUNT(d->io_base), count);
    outb(REG_LBA_LOW(d->io_base), lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (lba >> 16) & 0xFF);
    outb(REG_COMMAND(d->io_base), COMMAND_WRITE);

    for (uint8_t i = 0; i < count; i++) {
        if (!ide_wait_ready(d) || !(inb(REG_STATUS(d->io_base)) & STATUS_DRQ))
            return false;

        outsw(REG_DATA(d->io_base), b + (i * 512), 256);
    }

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
                ide->io_base =
                    (channel == 0) ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
            }

            if ((ctrl_bar & 1) == 1) {
                ide->ctrl_base = (uint16_t) (ctrl_bar & 0xFFFFFFFC);
            } else {
                ide->ctrl_base =
                    (channel == 0) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_IO;
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

bool ide_read_sector_wrapper(struct generic_disk *d, uint64_t lba, uint8_t *buf,
                             uint64_t cnt) {
    struct ide_drive *ide = d->driver_data;

    while (cnt > 0) {
        uint8_t chunk = (cnt >= 256) ? 0 : (uint8_t) cnt; // 0 means 256 sectors
        if (!ide_read_sector(ide, lba, buf, chunk))
            return false;

        uint64_t sectors = (chunk == 0) ? 256 : chunk;
        lba += sectors;
        buf += sectors * 512;
        cnt -= sectors;
    }

    return true;
}

bool ide_write_sector_wrapper(struct generic_disk *d, uint64_t lba,
                              const uint8_t *buf, uint64_t cnt) {
    struct ide_drive *ide = d->driver_data;

    while (cnt > 0) {
        uint8_t chunk = (cnt >= 256) ? 0 : (uint8_t) cnt;
        if (!ide_write_sector(ide, lba, buf, chunk))
            return false;

        uint64_t sectors = (chunk == 0) ? 256 : chunk;
        lba += sectors;
        buf += sectors * 512;
        cnt -= sectors;
    }

    return true;
}

void ide_print_info(struct generic_disk *d) {
    struct ide_drive *drive = (struct ide_drive *) d->driver_data;
    if (!drive->actually_exists)
        return;
    k_printf("IDE Drive identify:\n");
    k_printf("  IDE Drive Model: %s\n", drive->model);
    k_printf("  Serial: %s\n", drive->serial);
    k_printf("  Firmware: %s\n", drive->firmware);
    k_printf("  Sectors: %llu\n", drive->total_sectors);
    k_printf("  Size: %llu MB\n",
             (drive->total_sectors * drive->sector_size) / (1024 * 1024));
    k_printf("  LBA48: %s\n", drive->supports_lba48 ? "Yes" : "No");
    k_printf("  DMA: %s\n", drive->supports_dma ? "Yes" : "No");
    k_printf("  UDMA Mode: %u\n", drive->udma_mode);
    k_printf("  PIO Mode: %u\n", drive->pio_mode);
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

void ide_identify(struct ide_drive *drive) {
    uint16_t buf[256];
    uint16_t io = drive->io_base;

    outb(REG_DRIVE_HEAD(io), 0xA0 | (drive->slave ? 0x10 : 0x00));

    outb(REG_COMMAND(io), ATA_CMD_IDENTIFY);

    uint8_t status;
    while ((status = inb(REG_STATUS(io))) & STATUS_BSY)
        ;

    if (status == 0 || (status & STATUS_ERR)) {
        return;
    }

    while (!((status = inb(REG_STATUS(io))) & STATUS_DRQ))
        ;

    insw(REG_DATA(io), buf, 256);

#define copy_and_swap(dst, src, len)                                           \
    for (int i = 0; i < (len) / 2; ++i) {                                      \
        (dst)[2 * i] = ((src)[i] >> 8) & 0xFF;                                 \
        (dst)[2 * i + 1] = (src)[i] & 0xFF;                                    \
    }                                                                          \
    (dst)[len] = '\0';

    copy_and_swap(drive->serial, &buf[10], 20);
    copy_and_swap(drive->firmware, &buf[23], 8);
    copy_and_swap(drive->model, &buf[27], 40);

    for (int i = 39; i >= 0 && drive->model[i] == ' '; i--)
        drive->model[i] = '\0';

    drive->supports_lba48 = (buf[83] & (1 << 10)) ? 1 : 0;

    if (drive->supports_lba48) {
        drive->total_sectors =
            ((uint64_t) buf[100]) | ((uint64_t) buf[101] << 16) |
            ((uint64_t) buf[102] << 32) | ((uint64_t) buf[103] << 48);
    } else {
        drive->total_sectors =
            ((uint32_t) buf[60]) | ((uint32_t) buf[61] << 16);
    }

    drive->actually_exists = drive->total_sectors != 0;

    drive->sector_size = 512;

    drive->supports_dma = (buf[49] & (1 << 8)) ? 1 : 0;

    drive->udma_mode = 0;
    if (buf[88] & (1 << 13)) {
        for (int i = 0; i < 8; ++i) {
            if (buf[88] & (1 << i)) {
                drive->udma_mode = i;
            }
        }
    }

    drive->pio_mode = buf[64] & 0x03;
}
