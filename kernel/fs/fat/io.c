#include <console/printf.h>
#include <fs/fat.h>
#include <mem/alloc.h>
#include <string.h>

// TODO: errno :boom:

//
//
//
// ---------- WRITING CLUSTERS AND FAT ENTRIES ----------
//
//
//

bool fat_write_cluster(struct fat_fs *fs, uint32_t cluster,
                       const uint8_t *buffer) {
    const struct fat_bpb *bpb = fs->bpb;

    uint32_t lba = fat_cluster_to_lba(fs, cluster);

    return fs->disk->write_sector(fs->disk, lba, buffer,
                                  bpb->sectors_per_cluster);
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
    bool result = true;

    uint8_t *buf1 = kmalloc(disk->sector_size);
    uint8_t *buf2 = kmalloc(disk->sector_size);
    if (!buf1 || !buf2)
        return false;

    for (uint32_t fat_index = 0; fat_index < fs->bpb->num_fats; fat_index++) {
        uint32_t base = fs->bpb->reserved_sector_count + fat_index * fat_size;
        uint32_t sector = base + (fat_offset / fs->bpb->bytes_per_sector);
        sector += fs->volume_base_lba;

        if (!disk->read_sector(disk, sector, buf1, 1)) {
            result = false;
            continue;
        }

        if (offset == fs->bpb->bytes_per_sector - 1) {
            // Entry crosses a sector boundary
            if (!disk->read_sector(disk, sector + 1, buf2, 1)) {
                result = false;
                continue;
            }

            uint16_t combined = buf1[offset] | (buf2[0] << 8);
            if (cluster & 1)
                combined = (combined & 0x000F) | ((value & 0x0FFF) << 4);
            else
                combined = (combined & 0xF000) | (value & 0x0FFF);

            buf1[offset] = combined & 0xFF;
            buf2[0] = (combined >> 8) & 0xFF;

            if (!disk->write_sector(disk, sector, buf1, 1) ||
                !disk->write_sector(disk, sector + 1, buf2, 1)) {
                result = false;
            }
        } else {
            // Entry fits within a single sector
            uint16_t old = buf1[offset] | (buf1[offset + 1] << 8);
            uint16_t new_val;

            if (cluster & 1)
                new_val = (old & 0x000F) | ((value & 0x0FFF) << 4);
            else
                new_val = (old & 0xF000) | (value & 0x0FFF);

            buf1[offset] = new_val & 0xFF;
            buf1[offset + 1] = (new_val >> 8) & 0xFF;

            if (!disk->write_sector(disk, sector, buf1, 1)) {
                result = false;
            }
        }
    }

    kfree(buf1);
    kfree(buf2);
    return result;
}

// TODO: we can combine these with fat32 too
static bool fat16_write_fat_entry(struct fat_fs *fs, uint32_t cluster,
                                  uint32_t value) {
    struct generic_disk *disk = fs->disk;
    uint32_t fat_offset = cluster * 2;
    uint32_t offset = fat_offset % fs->bpb->bytes_per_sector;
    uint32_t fat_size = fs->bpb->fat_size_16;
    uint8_t *buf = kmalloc(disk->sector_size);
    if (!buf)
        return false;

    bool result = true;

    for (uint32_t fat_index = 0; fat_index < fs->bpb->num_fats; fat_index++) {
        uint32_t sector = fs->bpb->reserved_sector_count +
                          fat_index * fat_size +
                          (fat_offset / fs->bpb->bytes_per_sector);

        sector += fs->volume_base_lba;

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
    if (!buf)
        return false;

    bool result = true;

    for (uint32_t fat_index = 0; fat_index < fs->bpb->num_fats; fat_index++) {
        uint32_t sector = fs->bpb->reserved_sector_count +
                          fat_index * fat_size +
                          (fat_offset / fs->bpb->bytes_per_sector);

        sector += fs->volume_base_lba;

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

//
//
//
// ---------- READING CLUSTERS AND FAT ENTRIES ----------
//
//
//

bool fat_read_cluster(struct fat_fs *fs, uint32_t cluster, uint8_t *buffer) {
    const struct fat_bpb *bpb = fs->bpb;

    uint32_t lba = fat_cluster_to_lba(fs, cluster);
    return fs->disk->read_sector(fs->disk, lba, buffer,
                                 bpb->sectors_per_cluster);
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

    sector += fs->volume_base_lba;

    uint8_t *buf = kmalloc(disk->sector_size);
    uint8_t *buf2 = NULL;
    uint32_t result = 0xFFFFFFFF;
    if (!buf)
        return result;

    if (!disk->read_sector(disk, sector, buf, 1))
        goto done;

    if (offset == fs->bpb->bytes_per_sector - 1) {
        buf2 = kmalloc(disk->sector_size);
        if (!buf2)
            return result;

        if (!disk->read_sector(disk, sector + 1, buf2, 1))
            goto done;
        uint16_t val = buf[offset] | (buf2[0] << 8);
        result = (cluster & 1) ? (val >> 4) & 0x0FFF : val & 0x0FFF;
    } else {
        uint16_t val = buf[offset] | (buf[offset + 1] << 8);
        result = (cluster & 1) ? (val >> 4) & 0x0FFF : val & 0x0FFF;
    }

done:
    kfree(buf);
    if (buf2)
        kfree(buf2);
    return result;
}

// TODO: These are kinda same-y, can combine into one function

static uint32_t fat16_read_fat_entry(struct fat_fs *fs, uint32_t cluster) {
    struct generic_disk *disk = fs->disk;
    uint32_t fat_offset = cluster * 2;
    uint32_t offset = fat_offset % fs->bpb->bytes_per_sector;
    uint32_t sector = fs->bpb->reserved_sector_count +
                      (fat_offset / fs->bpb->bytes_per_sector);

    sector += fs->volume_base_lba;

    uint8_t *buf = kmalloc(disk->sector_size);
    uint32_t result = 0xFFFFFFFF;
    if (!buf)
        return result;

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

    sector += fs->volume_base_lba;

    uint8_t *buf = kmalloc(disk->sector_size);
    uint32_t result = 0xFFFFFFFF;
    if (!buf)
        return result;

    if (disk->read_sector(disk, sector, buf, 1))
        result = *(uint32_t *) &buf[offset] & 0x0FFFFFFF;

    kfree(buf);
    return result;
}

bool fat_write_dirent(struct fat_fs *fs, uint32_t dir_cluster,
                      const struct fat_dirent *dirent_to_write,
                      uint32_t entry_index) {

    uint32_t cluster_offset = entry_index / fs->entries_per_cluster;
    uint32_t index_in_cluster = entry_index % fs->entries_per_cluster;

    uint32_t current_cluster = dir_cluster;

    if (!(fs->type != FAT_32 && dir_cluster == FAT_DIR_CLUSTER_ROOT)) {
        for (uint32_t i = 0; i < cluster_offset; i++) {
            current_cluster = fat_read_fat_entry(fs, current_cluster);
            if (fat_is_eoc(fs, current_cluster)) {
                return false;
            }
        }
    }

    uint8_t *cluster_buf = kmalloc(fs->cluster_size);
    if (!cluster_buf)
        return false;

    if (!fat_read_cluster(fs, current_cluster, cluster_buf)) {
        kfree(cluster_buf);
        return false;
    }

    memcpy(cluster_buf + index_in_cluster * sizeof(struct fat_dirent),
           dirent_to_write, sizeof(struct fat_dirent));

    bool success = fat_write_cluster(fs, current_cluster, cluster_buf);

    kfree(cluster_buf);
    return success;
}
