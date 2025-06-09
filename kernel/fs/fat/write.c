#include <console/printf.h>
#include <fs/fat.h>
#include <mem/alloc.h>

// TODO: errno :boom:

bool fat_write_cluster(struct generic_disk *disk, uint32_t cluster,
                       const uint8_t *buffer) {
    struct fat_fs *fs = disk->fs_data;
    const struct fat_bpb *bpb = fs->bpb;

    uint32_t lba = fat_cluster_to_lba(fs, cluster);
    return disk->write_sector(disk, lba, buffer, bpb->sectors_per_cluster);
}

static bool fat12_write_fat_entry(struct fat_fs *fs, uint32_t, uint32_t value);
static bool fat16_write_fat_entry(struct fat_fs *fs, uint32_t, uint32_t value);
static bool fat32_write_fat_entry(struct fat_fs *fs, uint32_t, uint32_t value);

bool fat_write_fat_entry(struct fat_fs *fs, uint32_t cluster, uint32_t value) {
    switch (fs->type) {
    case FAT_12: return fat12_write_fat_entry(fs, cluster, value);
    case FAT_16: return fat16_write_fat_entry(fs, cluster, value);
    case FAT_32: return fat32_write_fat_entry(fs, cluster, value);
    }
    return false;
}

static bool fat12_write_fat_entry(struct fat_fs *fs, uint32_t cluster,
                                  uint32_t value) {
    struct generic_disk *disk = fs->disk;
    uint32_t fat_offset = cluster + (cluster / 2);
    uint32_t offset = fat_offset % fs->bpb->bytes_per_sector;
    uint32_t fat_size = fs->fat_size;
    uint8_t *buf1 = kmalloc(disk->sector_size);
    uint8_t *buf2 = kmalloc(disk->sector_size);
    bool result = true;

    for (uint32_t fat_index = 0; fat_index < fs->bpb->num_fats; fat_index++) {
        uint32_t base = fs->bpb->reserved_sector_count + fat_index * fat_size;
        uint32_t sector = base + (fat_offset / fs->bpb->bytes_per_sector);

        if (!disk->read_sector(disk, sector, buf1, 1)) {
            result = false;
            continue;
        }

        if (offset == fs->bpb->bytes_per_sector - 1) {
            if (!disk->read_sector(disk, sector + 1, buf2, 1)) {
                result = false;
                continue;
            }

            uint16_t combined = (buf2[0] << 8) | buf1[offset];
            if (cluster & 1)
                combined = (combined & 0x000F) | ((value & 0x0FFF) << 4);
            else
                combined = (combined & 0xF000) | (value & 0x0FFF);

            buf1[offset] = (combined >> 8) & 0xFF;
            buf2[0] = combined & 0xFF;

            if (!disk->write_sector(disk, sector, buf1, 1) ||
                !disk->write_sector(disk, sector + 1, buf2, 1)) {
                result = false;
            }
        } else {
            uint16_t *entry = (uint16_t *) &buf1[offset];
            uint16_t old = *entry;

            if (cluster & 1)
                *entry = (old & 0x000F) | ((value & 0x0FFF) << 4);
            else
                *entry = (old & 0xF000) | (value & 0x0FFF);

            if (!disk->write_sector(disk, sector, buf1, 1)) {
                result = false;
            }
        }
    }

    kfree(buf1);
    kfree(buf2);
    return result;
}

static bool fat16_write_fat_entry(struct fat_fs *fs, uint32_t cluster,
                                  uint32_t value) {
    struct generic_disk *disk = fs->disk;
    uint32_t fat_offset = cluster * 2;
    uint32_t offset = fat_offset % fs->bpb->bytes_per_sector;
    uint32_t fat_size = fs->bpb->fat_size_16;
    uint8_t *buf = kmalloc(disk->sector_size);
    bool result = true;

    for (uint32_t fat_index = 0; fat_index < fs->bpb->num_fats; fat_index++) {
        uint32_t sector = fs->bpb->reserved_sector_count +
                          fat_index * fat_size +
                          (fat_offset / fs->bpb->bytes_per_sector);

        if (!disk->read_sector(disk, sector, buf, 1)) {
            result = false;
            continue;
        }

        *(uint16_t *) &buf[offset] = value & 0xFFFF;

        if (!disk->write_sector(disk, sector, buf, 1)) {
            result = false;
        }
    }

    kfree(buf);
    return result;
}

static bool fat32_write_fat_entry(struct fat_fs *fs, uint32_t cluster,
                                  uint32_t value) {
    struct generic_disk *disk = fs->disk;
    uint32_t fat_offset = cluster * 4;
    uint32_t offset = fat_offset % fs->bpb->bytes_per_sector;
    uint32_t fat_size = fs->fat_size;
    uint8_t *buf = kmalloc(disk->sector_size);
    bool result = true;

    for (uint32_t fat_index = 0; fat_index < fs->bpb->num_fats; fat_index++) {
        uint32_t sector = fs->bpb->reserved_sector_count +
                          fat_index * fat_size +
                          (fat_offset / fs->bpb->bytes_per_sector);

        if (!disk->read_sector(disk, sector, buf, 1)) {
            result = false;
            continue;
        }

        uint32_t *entry = (uint32_t *) &buf[offset];
        *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);

        if (!disk->write_sector(disk, sector, buf, 1)) {
            result = false;
        }
    }

    kfree(buf);
    return result;
}

uint64_t fat_write_file(struct generic_disk *disk, uint32_t *start_cluster,
                        const uint8_t *data, uint64_t size) {
    struct fat_fs *fs = disk->fs_data;
    const struct fat_bpb *bpb = fs->bpb;
    uint32_t cluster_size = bpb->sectors_per_cluster * bpb->bytes_per_sector;

    uint32_t prev_cluster = 0;
    uint32_t written = 0;

    while (written < size) {
        uint32_t cluster = fat_alloc_cluster(fs);
        if (!cluster)
            return -1;

        if (*start_cluster == 0)
            *start_cluster = cluster;
        else
            fat_write_fat_entry(fs, prev_cluster, cluster);

        fat_write_fat_entry(fs, cluster, fat_eoc(fs));

        disk->write_sector(disk, fat_cluster_to_lba(fs, cluster),
                           (void *) (data + written), bpb->sectors_per_cluster);

        written += cluster_size;
        prev_cluster = cluster;
    }

    return written;
}
