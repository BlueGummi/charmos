#include <console/printf.h>
#include <fs/fat.h>
#include <mem/alloc.h>

bool fat_read_cluster(struct generic_disk *disk, uint32_t cluster,
                      uint8_t *buffer) {
    struct fat_fs *fs = disk->fs_data;
    const struct fat_bpb *bpb = fs->bpb;

    uint32_t lba = fat_cluster_to_lba(fs, cluster);
    return disk->read_sector(disk, lba, buffer, bpb->sectors_per_cluster);
}

static uint32_t fat12_read_fat_entry(struct fat_fs *fs, uint32_t cluster);
static uint32_t fat16_read_fat_entry(struct fat_fs *fs, uint32_t cluster);
static uint32_t fat32_read_fat_entry(struct fat_fs *fs, uint32_t cluster);

uint32_t fat_read_fat_entry(struct fat_fs *fs, uint32_t cluster) {
    switch (fs->type) {
    case FAT_12: return fat12_read_fat_entry(fs, cluster);
    case FAT_16: return fat16_read_fat_entry(fs, cluster);
    case FAT_32: return fat32_read_fat_entry(fs, cluster);
    }
    return 0xFFFFFFFF;
}

static uint32_t fat12_read_fat_entry(struct fat_fs *fs, uint32_t cluster) {
    struct generic_disk *disk = fs->disk;
    uint32_t fat_offset = cluster + (cluster / 2);
    uint32_t offset = fat_offset % fs->bpb->bytes_per_sector;
    uint32_t sector = fs->bpb->reserved_sector_count +
                      (fat_offset / fs->bpb->bytes_per_sector);
    uint8_t *buf = kmalloc(disk->sector_size);
    uint8_t *buf2 = NULL;
    uint32_t result = 0xFFFFFFFF;

    if (!disk->read_sector(disk, sector, buf, 1))
        goto done;

    if (offset == fs->bpb->bytes_per_sector - 1) {
        buf2 = kmalloc(disk->sector_size);
        if (!disk->read_sector(disk, sector + 1, buf2, 1))
            goto done;
        uint16_t val = (buf2[0] << 8) | buf[offset];
        result = (cluster & 1) ? (val >> 4) & 0x0FFF : val & 0x0FFF;
    } else {
        uint16_t val = *(uint16_t *) &buf[offset];
        result = (cluster & 1) ? (val >> 4) & 0x0FFF : val & 0x0FFF;
    }

done:
    kfree(buf);
    if (buf2)
        kfree(buf2);
    return result;
}

static uint32_t fat16_read_fat_entry(struct fat_fs *fs, uint32_t cluster) {
    struct generic_disk *disk = fs->disk;
    uint32_t fat_offset = cluster * 2;
    uint32_t offset = fat_offset % fs->bpb->bytes_per_sector;
    uint32_t sector = fs->bpb->reserved_sector_count +
                      (fat_offset / fs->bpb->bytes_per_sector);
    uint8_t *buf = kmalloc(disk->sector_size);
    uint32_t result = 0xFFFFFFFF;

    if (disk->read_sector(disk, sector, buf, 1))
        result = *(uint16_t *) &buf[offset];

    kfree(buf);
    return result;
}

static uint32_t fat32_read_fat_entry(struct fat_fs *fs, uint32_t cluster) {
    struct generic_disk *disk = fs->disk;
    uint32_t fat_offset = cluster * 4;
    uint32_t offset = fat_offset % fs->bpb->bytes_per_sector;
    uint32_t sector = fs->bpb->reserved_sector_count +
                      (fat_offset / fs->bpb->bytes_per_sector);
    uint8_t *buf = kmalloc(disk->sector_size);
    uint32_t result = 0xFFFFFFFF;

    if (disk->read_sector(disk, sector, buf, 1))
        result = *(uint32_t *) &buf[offset] & 0x0FFFFFFF;

    kfree(buf);
    return result;
}
