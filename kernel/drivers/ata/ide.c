#include <acpi/ioapic.h>
#include <asm.h>
#include <block/bcache.h>
#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <drivers/ata.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <sleep.h>
#include <stddef.h>
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
    uint16_t *buf = kmalloc(256 * sizeof(uint16_t));
    uint16_t io = drive->io_base;

    outb(REG_DRIVE_HEAD(io), 0xA0 | (drive->slave ? 0x10 : 0x00));

    outb(REG_COMMAND(io), ATA_CMD_IDENTIFY);

    uint8_t status;

    uint64_t timeout = IDE_IDENT_TIMEOUT_MS * 1000;
    while ((status = inb(REG_STATUS(io))) & STATUS_BSY) {
        sleep_us(10);
        timeout--;
        if (timeout == 0)
            goto out;
    }

    if (status == 0 || (status & STATUS_ERR)) {
        goto out;
    }

    timeout = IDE_IDENT_TIMEOUT_MS * 1000;
    while (!((status = inb(REG_STATUS(io))) & STATUS_DRQ)) {
        sleep_us(10);
        timeout--;
        if (timeout == 0)
            goto out;
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
out:
    kfree(buf);
}

static struct bio_scheduler_ops ide_bio_ops = {
    .should_coalesce = noop_should_coalesce,
    .reorder = ide_reorder,
    .do_coalesce = noop_do_coalesce,

    .max_wait_time =
        {
            [BIO_RQ_BACKGROUND] = 100,
            [BIO_RQ_LOW] = 75,
            [BIO_RQ_MEDIUM] = 50,
            [BIO_RQ_HIGH] = 25,
            [BIO_RQ_URGENT] = 0,
        },

    .dispatch_threshold = 64,

    .boost_occupance_limit =
        {
            [BIO_RQ_BACKGROUND] = 32,
            [BIO_RQ_LOW] = 24,
            [BIO_RQ_MEDIUM] = 16,
            [BIO_RQ_HIGH] = 8,
            [BIO_RQ_URGENT] = 0,
        },
    .min_wait_ms = 2,
    .tick_ms = 35,
};

struct generic_disk *ide_create_generic(struct ata_drive *ide) {
    ide_identify(ide);
    if (!ide->actually_exists)
        return NULL;

    uint8_t irq = idt_alloc_entry();
    k_info("IDE", K_INFO, "IDE drive IRQ on line %u, allocated entry %u",
           ide->irq, irq);

    ioapic_route_irq(ide->irq, irq, 0, false);
    isr_register(irq, ide_irq_handler, &ide->channel, 0);
    ide->channel.current_drive = ide;

    struct generic_disk *d = kmalloc(sizeof(struct generic_disk));
    d->driver_data = ide;
    d->sector_size = ide->sector_size;
    d->read_sector = ide_read_sector_wrapper;
    d->write_sector = ide_write_sector_wrapper;
    d->submit_bio_async = ide_submit_bio_async;

    d->print = ide_print_info;
    d->flags = DISK_FLAG_NO_COALESCE | DISK_FLAG_NO_REORDER;

    d->cache = kzalloc(sizeof(struct bcache));
    d->scheduler = bio_sched_create(d, &ide_bio_ops);

    bcache_init(d->cache, DEFAULT_BLOCK_CACHE_SIZE);

    d->type = G_IDE_DRIVE;
    return d;
}
