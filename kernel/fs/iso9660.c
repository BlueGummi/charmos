#include <block/generic.h>
#include <console/printf.h>
#include <fs/iso9660.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <time.h>

bool iso9660_read_file(struct iso9660_fs *fs, uint32_t lba, uint32_t size,
                       void *out_buf) {
    uint32_t num_blocks = (size + fs->block_size - 1) / fs->block_size;

    if (!fs->disk->read_sector(fs->disk, lba + fs->partition->start_lba,
                               out_buf, num_blocks)) {
        return false;
    }

    return true;
}

bool iso9660_parse_pvd(struct generic_partition *p,
                       struct iso9660_pvd *out_pvd) {
    uint8_t *buffer = kmalloc(ISO9660_SECTOR_SIZE, ALLOC_PARAMS_DEFAULT);
    if (!buffer)
        return false;

    struct generic_disk *disk = p->disk;

    if (!disk->read_sector(disk, ISO9660_PVD_SECTOR + p->start_lba, buffer,
                           1)) {
        printf("Failed to read ISO9660 PVD sector\n");
        kfree(buffer, FREE_PARAMS_DEFAULT);
        return false;
    }

    struct iso9660_pvd *pvd = (struct iso9660_pvd *) buffer;

    if (pvd->type != 1 || strncmp(pvd->id, "CD001", 5) != 0 ||
        pvd->version != 1) {
        printf("Not a valid Primary Volume Descriptor\n");
        kfree(buffer, FREE_PARAMS_DEFAULT);
        return false;
    }

    memcpy(out_pvd, pvd, sizeof(struct iso9660_pvd));

    kfree(buffer, FREE_PARAMS_DEFAULT);
    return true;
}

static void print_str(const char *label, const char *src, uint64_t len) {
    char buf[65] = {0};
    memcpy(buf, src, len);
    buf[len] = '\0';
    printf("%s: \"%s\"\n", label, buf);
}

void iso9660_pvd_print(const struct iso9660_pvd *pvd) {
    printf("=== ISO9660 Primary Volume Descriptor ===\n");
    printf("Descriptor Type: %u\n", pvd->type);
    printf("Identifier: %-5s\n", pvd->id);
    printf("Version: %u\n", pvd->version);

    print_str("System Identifier", pvd->system_id, 32);
    print_str("Volume Identifier", pvd->volume_id, 32);

    printf("Volume Space Size: %u blocks\n", pvd->volume_space_le);
    printf("Logical Block Size: %u bytes\n", pvd->logical_block_size_le);

    printf("Volume Set Size: %u\n", pvd->vol_set_size_le);
    printf("Volume Sequence Number: %u\n", pvd->vol_seq_num_le);
    printf("Path Table Size: %u bytes\n", pvd->path_table_size_le);

    printf("L Path Table Location: 0x%08X\n", pvd->l_path_table_loc);
    printf("Optional L Path Table Location: 0x%08X\n",
             pvd->opt_l_path_table_loc);
    printf("M Path Table Location: 0x%08X\n", pvd->m_path_table_loc);
    printf("Optional M Path Table Location: 0x%08X\n",
             pvd->opt_m_path_table_loc);
}

struct vfs_node *iso9660_mount(struct generic_partition *p) {
    struct iso9660_pvd pvd;
    struct generic_disk *disk = p->disk;
    if (iso9660_parse_pvd(p, &pvd)) {
        struct iso9660_fs *fs =
            kzalloc(sizeof(struct iso9660_fs), ALLOC_PARAMS_DEFAULT);
        struct iso9660_pvd *new_pvd =
            kzalloc(sizeof(struct iso9660_pvd), ALLOC_PARAMS_DEFAULT);
        if (!fs || !new_pvd)
            return NULL;

        fs->pvd = new_pvd;
        memcpy(fs->pvd, &pvd, sizeof(struct iso9660_pvd));
        fs->root_lba = pvd.root_dir_record.extent_lba_le;
        fs->root_size = pvd.root_dir_record.size_le;
        fs->disk = disk;
        fs->partition = p;
        fs->block_size = pvd.logical_block_size_le;
        disk->fs_data = fs;
        p->fs_data = fs;
        return NULL;
    }
    return NULL;
}

void iso9660_ls(struct iso9660_fs *fs, uint32_t lba, uint32_t size) {
    uint32_t num_blocks = (size + fs->block_size - 1) / fs->block_size;
    uint8_t *dir_data =
        kmalloc(num_blocks * fs->block_size, ALLOC_PARAMS_DEFAULT);
    if (!dir_data)
        return;

    if (!fs->disk->read_sector(fs->disk, lba + fs->partition->start_lba,
                               dir_data, num_blocks)) {
        kfree(dir_data, FREE_PARAMS_DEFAULT);
        return;
    }

    uint64_t offset = 0;
    while (offset < size) {
        struct iso9660_dir_record *rec =
            (struct iso9660_dir_record *) (dir_data + offset);

        if (rec->length == 0) {
            offset = ((offset / fs->block_size) + 1) * fs->block_size;
            continue;
        }

        if (rec->name_len == 1 && (rec->name[0] == 0 || rec->name[0] == 1)) {
            offset += rec->length;
            continue;
        }

        char name[256] = {0};
        memcpy(name, rec->name, rec->name_len);
        name[rec->name_len] = '\0';

        printf("  %s  (LBA: %u, Size: %u bytes, %s)\n", name,
                 rec->extent_lba_le, rec->size_le,
                 (rec->flags & 0x02) ? "Directory" : "File");
        if (rec->flags & 0x02) {
            printf("--Listing %s--\n", name);
            iso9660_ls(fs, rec->extent_lba_le, rec->size_le);
        }

        offset += rec->length;
    }
    kfree(dir_data, FREE_PARAMS_DEFAULT);
}

void iso9660_print(struct generic_partition *disk) {
    struct iso9660_pvd pvd;
    struct iso9660_fs *fs = disk->fs_data;
    if (!iso9660_parse_pvd(disk, &pvd)) {
        return;
    }

    iso9660_pvd_print(&pvd);

    iso9660_ls(fs, fs->root_lba, fs->root_size);
}

struct iso9660_dir_record *iso9660_find(struct iso9660_fs *fs,
                                        const char *target_name, uint32_t lba,
                                        uint32_t size) {
    uint32_t num_blocks = (size + fs->block_size - 1) / fs->block_size;
    uint8_t *dir_data =
        kmalloc(num_blocks * fs->block_size, ALLOC_PARAMS_DEFAULT);
    if (!dir_data)
        return NULL;

    if (!fs->disk->read_sector(fs->disk, lba + fs->partition->start_lba,
                               dir_data, num_blocks)) {
        kfree(dir_data, FREE_PARAMS_DEFAULT);
        return NULL;
    }

    uint64_t offset = 0;
    while (offset < size) {
        struct iso9660_dir_record *rec =
            (struct iso9660_dir_record *) (dir_data + offset);
        if (rec->length == 0) {
            offset = ((offset / fs->block_size) + 1) * fs->block_size;
            continue;
        }

        if (!(rec->name_len == 1 && (rec->name[0] == 0 || rec->name[0] == 1))) {
            char name[256] = {0};
            memcpy(name, rec->name, rec->name_len);
            name[rec->name_len] = '\0';

            if (strcmp(name, target_name) == 0) {
                struct iso9660_dir_record *found = kmalloc(
                    sizeof(struct iso9660_dir_record), ALLOC_PARAMS_DEFAULT);
                if (!found)
                    return NULL;

                memcpy(found, rec, sizeof(struct iso9660_dir_record));
                kfree(dir_data, FREE_PARAMS_DEFAULT);
                return found;
            }
        }

        offset += rec->length;
    }

    kfree(dir_data, FREE_PARAMS_DEFAULT);
    return NULL;
}

void iso9660_read_and_print_file(struct iso9660_fs *fs, const char *name) {
    struct iso9660_dir_record *rec =
        iso9660_find(fs, name, fs->root_lba, fs->root_size);
    if (!rec) {
        printf("File '%s' not found\n", name);
        return;
    }

    if (rec->flags & 0x02) {
        printf("'%s' is a directory, not a file\n", name);
        kfree(rec, FREE_PARAMS_DEFAULT);
        return;
    }

    void *buf = kmalloc(rec->size_le, ALLOC_PARAMS_DEFAULT);
    if (!buf)
        return;

    if (!iso9660_read_file(fs, rec->extent_lba_le, rec->size_le, buf)) {
        printf("Failed to read file contents\n");
        kfree(buf, FREE_PARAMS_DEFAULT);
        kfree(rec, FREE_PARAMS_DEFAULT);
        return;
    }

    printf("Contents of '%s':\n%.*s\n", name, rec->size_le, (char *) buf);

    kfree(buf, FREE_PARAMS_DEFAULT);
    kfree(rec, FREE_PARAMS_DEFAULT);
}

struct iso9660_datetime iso9660_get_current_date(void) {
    struct iso9660_datetime dt;

    dt.year = (time_get_century() * 100 + time_get_year()) - 1900;
    dt.month = time_get_month();
    dt.day = time_get_day();
    dt.hour = time_get_hour();
    dt.minute = time_get_minute();
    dt.second = time_get_second();

    dt.gmt_offset =
        0; // TODO: once we have better stuff up and going - alter this

    return dt;
}
