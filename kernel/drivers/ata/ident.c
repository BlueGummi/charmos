#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/ide.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <pci/pci.h>
#include <stdbool.h>
#include <stdint.h>

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
