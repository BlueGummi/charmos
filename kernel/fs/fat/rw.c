#include <console/printf.h>
#include <fs/fat.h>
#include <mem/alloc.h>

// TODO: errno :boom:

uint32_t fat_first_data_sector(const struct fat_fs *fs) {
    const struct fat_bpb *bpb = fs->bpb;

    uint32_t root_dir_sectors =
        ((bpb->root_entry_count * 32) + (bpb->bytes_per_sector - 1)) /
        bpb->bytes_per_sector;

    return bpb->reserved_sector_count + (bpb->num_fats * fs->fat_size) +
           ((fs->type == FAT_12 || fs->type == FAT_16) ? root_dir_sectors : 0);
}

uint32_t fat_cluster_to_lba(const struct fat_fs *fs, uint32_t cluster) {
    const struct fat_bpb *bpb = fs->bpb;
    return fat_first_data_sector(fs) + (cluster - 2) * bpb->sectors_per_cluster;
}

bool fat_read_cluster(struct generic_disk *disk, uint32_t cluster,
                      uint8_t *buffer) {
    struct fat_fs *fs = disk->fs_data;
    const struct fat_bpb *bpb = fs->bpb;

    uint32_t lba = fat_cluster_to_lba(fs, cluster);
    return disk->read_sector(disk, lba, buffer, bpb->sectors_per_cluster);
}

bool fat_write_cluster(struct generic_disk *disk, uint32_t cluster,
                       const uint8_t *buffer) {
    struct fat_fs *fs = disk->fs_data;
    const struct fat_bpb *bpb = fs->bpb;

    uint32_t lba = fat_cluster_to_lba(fs, cluster);
    return disk->write_sector(disk, lba, buffer, bpb->sectors_per_cluster);
}

uint32_t fat_read_fat_entry(struct fat_fs *fs, uint32_t cluster) {
    struct generic_disk *disk = fs->disk;
    uint32_t fat_start = fs->bpb->reserved_sector_count;
    uint32_t fat_offset, sector, offset;
    uint8_t *buf = kmalloc(disk->sector_size);

    switch (fs->type) {
    case FAT_12:
        fat_offset = cluster + (cluster / 2); // 1.5 bytes per entry
        sector = fat_start + (fat_offset / fs->bpb->bytes_per_sector);
        offset = fat_offset % fs->bpb->bytes_per_sector;

        if (!disk->read_sector(disk, sector, buf, 1))
            return 0xFFFFFFFF;

        if (offset == fs->bpb->bytes_per_sector - 1) {
            uint8_t b1 = buf[offset];
            if (!disk->read_sector(disk, sector + 1, buf, 1))
                return 0xFFFFFFFF;
            uint8_t b2 = buf[0];
            uint16_t val = (b2 << 8) | b1;
            return (cluster & 1) ? (val >> 4) & 0xFFF : val & 0xFFF;
        } else {
            uint16_t val = *(uint16_t *) &buf[offset];
            return (cluster & 1) ? (val >> 4) & 0xFFF : val & 0xFFF;
        }

    case FAT_16:
        fat_offset = cluster * 2;
        sector = fat_start + (fat_offset / fs->bpb->bytes_per_sector);
        offset = fat_offset % fs->bpb->bytes_per_sector;

        if (!disk->read_sector(disk, sector, buf, 1))
            return 0xFFFFFFFF;
        return *(uint16_t *) &buf[offset];

    case FAT_32:
        fat_offset = cluster * 4;
        sector = fat_start + (fat_offset / fs->bpb->bytes_per_sector);
        offset = fat_offset % fs->bpb->bytes_per_sector;

        if (!disk->read_sector(disk, sector, buf, 1))
            return 0xFFFFFFFF;
        return *(uint32_t *) &buf[offset] & 0x0FFFFFFF;
    }

    return 0xFFFFFFFF;
}

bool fat_write_fat_entry(struct fat_fs *fs, uint32_t cluster, uint32_t value) {
    struct generic_disk *disk = fs->disk;
    uint32_t fat_start = fs->bpb->reserved_sector_count;
    uint32_t fat_offset, sector, offset;
    uint8_t *buf = kmalloc(disk->sector_size);

    switch (fs->type) {
    case FAT_12:
        fat_offset = cluster + (cluster / 2);
        sector = fat_start + (fat_offset / fs->bpb->bytes_per_sector);
        offset = fat_offset % fs->bpb->bytes_per_sector;

        if (!disk->read_sector(disk, sector, buf, 1))
            return false;

        if (offset == fs->bpb->bytes_per_sector - 1) {
            uint8_t b1 = buf[offset];
            if (!disk->read_sector(disk, sector + 1, buf, 1))
                return false;
            uint8_t b2 = buf[0];

            uint16_t combined = (b2 << 8) | b1;
            if (cluster & 1)
                combined = (combined & 0x000F) | ((value & 0x0FFF) << 4);
            else
                combined = (combined & 0xF000) | (value & 0x0FFF);

            buf[0] = combined & 0xFF;
            if (!disk->write_sector(disk, sector + 1, buf, 1))
                return false;
            if (!disk->read_sector(disk, sector, buf, 1))
                return false;
            buf[offset] = (combined >> 8) & 0xFF;
            return disk->write_sector(disk, sector, buf, 1);
        } else {
            uint16_t *entry = (uint16_t *) &buf[offset];
            uint16_t old = *entry;
            if (cluster & 1)
                *entry = (old & 0x000F) | ((value & 0x0FFF) << 4);
            else
                *entry = (old & 0xF000) | (value & 0x0FFF);
            return disk->write_sector(disk, sector, buf, 1);
        }

    case FAT_16:
        fat_offset = cluster * 2;
        sector = fat_start + (fat_offset / fs->bpb->bytes_per_sector);
        offset = fat_offset % fs->bpb->bytes_per_sector;

        if (!disk->read_sector(disk, sector, buf, 1))
            return false;
        *(uint16_t *) &buf[offset] = value & 0xFFFF;
        return disk->write_sector(disk, sector, buf, 1);

    case FAT_32:
        fat_offset = cluster * 4;
        sector = fat_start + (fat_offset / fs->bpb->bytes_per_sector);
        offset = fat_offset % fs->bpb->bytes_per_sector;

        if (!disk->read_sector(disk, sector, buf, 1))
            return false;
        uint32_t *entry = (uint32_t *) &buf[offset];
        *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);
        return disk->write_sector(disk, sector, buf, 1);
    }

    return false;
}

uint32_t fat_alloc_cluster(struct fat_fs *fs) {
    uint32_t total_clusters = fs->total_clusters;
    for (uint32_t cluster = 2; cluster < total_clusters + 2; cluster++) {
        uint32_t entry = fat_read_fat_entry(fs, cluster);
        if (entry == 0x00000000) {
            fat_write_fat_entry(fs, cluster, fat_eoc(fs));
            return cluster;
        }
    }
    return 0;
}

void fat_free_chain(struct fat_fs *fs, uint32_t start_cluster) {
    uint32_t cluster = start_cluster;
    while (!fat_is_eoc(fs, cluster)) {
        uint32_t next = fat_read_fat_entry(fs, cluster);
        fat_write_fat_entry(fs, cluster, 0x00000000);
        if (next == cluster)
            break; // prevent loop from going on and on
        cluster = next;
    }
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
