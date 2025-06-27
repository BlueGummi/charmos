#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/ata.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <pci/pci.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

void ide_print_info(struct generic_disk *d) {
    struct ata_drive *drive = (struct ata_drive *) d->driver_data;
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

static void swap_str(char *dst, const uint16_t *src, uint64_t word_len) {
    for (uint64_t i = 0; i < word_len; ++i) {
        dst[2 * i] = (src[i] >> 8) & 0xFF;
        dst[2 * i + 1] = src[i] & 0xFF;
    }
    dst[2 * word_len] = '\0';

    for (int i = 2 * word_len - 1; i >= 0; --i) {
        if (dst[i] == ' ' || dst[i] == '\0')
            dst[i] = '\0';
        else
            break;
    }
}

void ide_identify(struct ata_drive *drive) {
    uint16_t buf[256];
    uint16_t io = drive->io_base;

    outb(REG_DRIVE_HEAD(io), 0xA0 | (drive->slave ? 0x10 : 0x00));

    outb(REG_COMMAND(io), ATA_CMD_IDENTIFY);

    uint8_t status;

    uint64_t timeout = IDE_IDENT_TIMEOUT_MS * 1000;
    while ((status = inb(REG_STATUS(io))) & STATUS_BSY) {
        sleep_us(10);
        timeout--;
        if (timeout == 0)
            return;
    }

    if (status == 0 || (status & STATUS_ERR)) {
        return;
    }

    timeout = IDE_IDENT_TIMEOUT_MS * 1000;
    while (!((status = inb(REG_STATUS(io))) & STATUS_DRQ)) {
        sleep_us(10);
        timeout--;
        if (timeout == 0)
            return;
    }

    insw(REG_DATA(io), buf, 256);

    swap_str(drive->serial, &buf[10], 10);
    swap_str(drive->firmware, &buf[23], 4);
    swap_str(drive->model, &buf[27], 20);

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
