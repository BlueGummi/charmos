#include <console/printf.h>
#include <fs/fat.h>
#include <mem/alloc.h>
#include <string.h>

bool fat_create_file_in_dir(struct generic_disk *disk, uint32_t dir_cluster,
                            const char *filename,
                            struct fat_dirent *out_dirent) {

    struct fat_fs *fs = disk->fs_data;

    uint32_t new_cluster = fat_alloc_cluster(fs);
    if (new_cluster == 0) {
        k_printf("could not alloc\n");
        return false;
    }

    uint32_t cluster_size =
        fs->bpb->sectors_per_cluster * fs->bpb->bytes_per_sector;
    uint8_t *cluster_buf = kzalloc(cluster_size);
    if (!fat_write_cluster(disk, new_cluster, cluster_buf)) {
        kfree(cluster_buf);
        k_printf("could not write\n");
        return false;
    }
    kfree(cluster_buf);

    uint8_t *dir_buf = kmalloc(cluster_size);
    if (!dir_buf)
        return false;

    uint32_t current_cluster = dir_cluster;
    uint32_t prev_cluster = 0;
    bool found_slot = false;
    uint32_t slot_offset = 0;

    while (current_cluster != fat_eoc(fs)) {
        if (!fat_read_cluster(disk, current_cluster, dir_buf)) {
            k_printf("could not read here\n");
            kfree(dir_buf);
            return false;
        }

        for (uint32_t offset = 0; offset < cluster_size;
             offset += sizeof(struct fat_dirent)) {
            struct fat_dirent *ent = (struct fat_dirent *) (dir_buf + offset);
            if (ent->name[0] == 0x00 || (uint8_t) ent->name[0] == 0xE5) {
                found_slot = true;
                slot_offset = offset;
                break;
            }
        }
        if (found_slot)
            break;

        prev_cluster = current_cluster;
        current_cluster = fat_read_fat_entry(fs, current_cluster);
    }

    if (!found_slot && fs->type == FAT_32) {
        uint32_t new_dir_cluster = fat_alloc_cluster(fs);
        if (new_dir_cluster == 0) {
            kfree(dir_buf);
            return false; // no free clusters to extend directory
        }

        cluster_buf = kzalloc(cluster_size);
        if (!fat_write_cluster(disk, new_dir_cluster, cluster_buf)) {
            kfree(cluster_buf);
            kfree(dir_buf);
            return false;
        }
        kfree(cluster_buf);

        if (prev_cluster != 0) {
            fat_write_fat_entry(fs, prev_cluster, new_dir_cluster);
        } else {
            dir_cluster = new_dir_cluster;
        }
        fat_write_fat_entry(fs, new_dir_cluster, fat_eoc(fs));

        current_cluster = new_dir_cluster;
        slot_offset = 0;
        found_slot = true;

        if (!fat_read_cluster(disk, current_cluster, dir_buf)) {
            kfree(dir_buf);
            return false;
        }
    }

    if (!found_slot) {
        kfree(dir_buf);
        return false;
    }

    struct fat_dirent *new_ent = (struct fat_dirent *) (dir_buf + slot_offset);
    memset(new_ent, 0, sizeof(struct fat_dirent));
    fat_format_filename_83(filename, new_ent->name);
    new_ent->attr = 0x20;

    new_ent->low_cluster = new_cluster & 0xFFFF;
    new_ent->high_cluster = (new_cluster >> 16) & 0xFFFF;

    new_ent->filesize = 0;

    struct fat_date date = fat_get_current_date();
    struct fat_time time = fat_get_current_time();

    new_ent->crtdate = date;
    new_ent->crttime = time;
    new_ent->lastaccess = date;
    new_ent->moddate = date;
    new_ent->modtime = time;

    bool ret = fat_write_cluster(disk, current_cluster, dir_buf);
    kfree(dir_buf);

    if (ret && out_dirent) {
        memcpy(out_dirent, new_ent, sizeof(struct fat_dirent));
    }

    return ret;
}
