#include <fs/fat.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// TODO: failure - ENOSPC

static inline uint32_t fat12_16_root_dir_lba(struct fat_fs *fs) {
    return fs->bpb->reserved_sector_count +
           (fs->bpb->num_fats * fs->bpb->fat_size_16) + fs->volume_base_lba;
}

static inline uint32_t fat12_16_root_dir_sectors(struct fat_fs *fs) {
    return ((fs->bpb->root_entry_count * 32) + fs->bpb->bytes_per_sector - 1) /
           fs->bpb->bytes_per_sector;
}

static bool fat_find_free_dirent_slot(struct fat_fs *fs, uint32_t dir_cluster,
                                      uint8_t *dir_buf, uint32_t *out_cluster,
                                      uint32_t *out_offset,
                                      uint32_t *out_prev_cluster) {

    if (dir_cluster == FAT_DIR_CLUSTER_ROOT && fs->type != FAT_32) {
        uint32_t root_lba = fat12_16_root_dir_lba(fs);
        uint32_t root_secs = fat12_16_root_dir_sectors(fs);
        for (uint32_t i = 0; i < root_secs; ++i) {
            if (!fs->disk->read_sector(fs->disk, root_lba + i, dir_buf, 1))
                return false;

            for (uint32_t offset = 0; offset < fs->bpb->bytes_per_sector;
                 offset += sizeof(struct fat_dirent)) {
                struct fat_dirent *ent =
                    (struct fat_dirent *) (dir_buf + offset);
                if (ent->name[0] == 0x00 || (uint8_t) ent->name[0] == 0xE5) {
                    *out_cluster = i;
                    *out_offset = offset;
                    return true;
                }
            }
        }

        return false;
    }

    uint32_t current_cluster = dir_cluster;
    *out_prev_cluster = 0;

    while (current_cluster != fat_eoc(fs)) {
        if (!fat_read_cluster(fs, current_cluster, dir_buf))
            return false;

        for (uint32_t offset = 0; offset < fs->cluster_size;
             offset += sizeof(struct fat_dirent)) {
            struct fat_dirent *ent = (struct fat_dirent *) (dir_buf + offset);
            if (ent->name[0] == 0x00 || (uint8_t) ent->name[0] == 0xE5) {
                *out_cluster = current_cluster;
                *out_offset = offset;
                return true;
            }
        }

        *out_prev_cluster = current_cluster;
        current_cluster = fat_read_fat_entry(fs, current_cluster);
    }

    return false;
}

static bool fat_extend_directory(struct fat_fs *fs, uint32_t prev_cluster,
                                 uint32_t *new_cluster_out, uint8_t *dir_buf) {
    if (prev_cluster == FAT_DIR_CLUSTER_ROOT && fs->type != FAT_32) {
        // can't extend FAT12/16 root directory
        return false;
    }

    uint32_t new_cluster = fat_alloc_cluster(fs);
    if (new_cluster == 0)
        return false;

    uint8_t *cluster_buf = kzalloc(fs->cluster_size);
    if (!fat_write_cluster(fs, new_cluster, cluster_buf)) {
        kfree(cluster_buf);
        return false;
    }
    kfree(cluster_buf);

    if (prev_cluster)
        fat_write_fat_entry(fs, prev_cluster, new_cluster);
    fat_write_fat_entry(fs, new_cluster, fat_eoc(fs));

    if (!fat_read_cluster(fs, new_cluster, dir_buf))
        return false;

    *new_cluster_out = new_cluster;
    return true;
}

static void fat_initialize_dirent(struct fat_dirent *ent, const char *filename,
                                  uint32_t cluster, enum fat_fileattr attr) {
    memset(ent, 0, sizeof(struct fat_dirent));

    if (strcmp(filename, ".") == 0) {
        memcpy(ent->name, ".          ", 11);
    } else if (strcmp(filename, "..") == 0) {
        memcpy(ent->name, "..         ", 11);
    } else {
        fat_format_filename_83(filename, ent->name);
    }

    ent->attr = attr;

    ent->low_cluster = cluster & 0xFFFF;
    ent->high_cluster = cluster >> 16;

    ent->filesize = 0;

    struct fat_date date = fat_get_current_date();
    struct fat_time time = fat_get_current_time();

    ent->crtdate = date;
    ent->crttime = time;
    ent->lastaccess = date;
    ent->moddate = date;
    ent->modtime = time;
}

bool fat_create(struct fat_fs *fs, uint32_t dir_cluster, const char *filename,
                struct fat_dirent *out_dirent, enum fat_fileattr attr,
                uint32_t *out_cluster) {
    if (!dir_cluster)
        return false;

    if (fat_contains(fs, dir_cluster, filename))
        return false;

    uint8_t *dir_buf = kmalloc(fs->cluster_size);
    if (!dir_buf)
        return false;

    uint32_t slot_cluster = 0, slot_offset = 0, prev_cluster = 0;

    bool found = fat_find_free_dirent_slot(
        fs, dir_cluster, dir_buf, &slot_cluster, &slot_offset, &prev_cluster);

    if (!found && fs->type == FAT_32) {
        if (!fat_extend_directory(fs, prev_cluster, &slot_cluster, dir_buf)) {
            kfree(dir_buf);
            return false;
        }
        slot_offset = 0;
        found = true;
    }

    if (!found) {
        kfree(dir_buf);
        return false;
    }

    uint32_t cluster = 0;

    bool needs_cluster = (attr == FAT_DIR && strcmp(filename, ".") != 0 &&
                          strcmp(filename, "..") != 0);

    if (needs_cluster)
        cluster = fat_alloc_cluster(fs);

    if (out_cluster)
        *out_cluster = cluster;

    struct fat_dirent *new_ent = (struct fat_dirent *) (dir_buf + slot_offset);
    fat_initialize_dirent(new_ent, filename, cluster, attr);

    bool success = false;
    if (dir_cluster == FAT_DIR_CLUSTER_ROOT && fs->type != FAT_32) {
        uint32_t root_lba = fat12_16_root_dir_lba(fs);
        success = fs->disk->write_sector(fs->disk, root_lba + slot_cluster,
                                         dir_buf, 1);
    } else {
        success = fat_write_cluster(fs, slot_cluster, dir_buf);
    }

    kfree(dir_buf);

    if (success && out_dirent)
        memcpy(out_dirent, new_ent, sizeof(struct fat_dirent));

    return success;
}

bool fat_mkdir(struct fat_fs *fs, uint32_t parent_cluster, const char *name,
               struct fat_dirent *out_dirent) {
    struct fat_dirent new_dirent;
    uint32_t new_cluster = 0;

    if (!fat_create(fs, parent_cluster, name, &new_dirent, FAT_DIR,
                    &new_cluster))
        return false;

    *out_dirent = new_dirent;

    uint8_t *buf = kzalloc(fs->cluster_size);
    if (!buf)
        return false;

    struct fat_dirent *dot = (struct fat_dirent *) buf;
    fat_initialize_dirent(dot, ".", new_cluster, FAT_DIR);

    struct fat_dirent *dotdot =
        (struct fat_dirent *) (buf + sizeof(struct fat_dirent));

    fat_initialize_dirent(
        dotdot, "..", parent_cluster == fs->root_cluster ? 0 : parent_cluster,
        FAT_DIR);

    bool ok = fat_write_cluster(fs, new_cluster, buf);
    kfree(buf);
    return ok;
}
