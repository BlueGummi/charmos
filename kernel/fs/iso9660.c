#include <console/printf.h>
#include <devices/generic_disk.h>
#include <errno.h>
#include <fs/iso9660.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <string.h>

bool iso9660_parse_pvd(struct generic_disk *disk, struct iso9660_pvd *out_pvd) {
    uint8_t *buffer = kmalloc(ISO9660_SECTOR_SIZE);

    if (!disk->read_sector(disk, ISO9660_PVD_SECTOR, buffer, 1)) {
        k_printf("Failed to read ISO9660 PVD sector\n");
        kfree(buffer);
        return false;
    }

    struct iso9660_pvd *pvd = (struct iso9660_pvd *) buffer;

    if (pvd->type != 1 || strncmp(pvd->id, "CD001", 5) != 0 ||
        pvd->version != 1) {
        k_printf("Not a valid Primary Volume Descriptor\n");
        kfree(buffer);
        return false;
    }

    memcpy(out_pvd, pvd, sizeof(struct iso9660_pvd));

    k_printf("ISO9660 Volume ID: %-32s\n", out_pvd->volume_id);
    k_printf("Logical Block Size: %u\n", out_pvd->logical_block_size_le);
    k_printf("Volume Space (blocks): %u\n", out_pvd->volume_space_le);

    kfree(buffer);
    return true;
}

static void print_str(const char *label, const char *src, size_t len) {
    char buf[65] = {0};
    memcpy(buf, src, len);
    buf[len] = '\0';
    k_printf("%s: \"%s\"\n", label, buf);
}

void iso9660_pvd_print(const struct iso9660_pvd *pvd) {
    k_printf("=== ISO9660 Primary Volume Descriptor ===\n");
    k_printf("Descriptor Type: %u\n", pvd->type);
    k_printf("Identifier: %-5s\n", pvd->id);
    k_printf("Version: %u\n", pvd->version);

    print_str("System Identifier", pvd->system_id, 32);
    print_str("Volume Identifier", pvd->volume_id, 32);

    k_printf("Volume Space Size: %u blocks\n", pvd->volume_space_le);
    k_printf("Logical Block Size: %u bytes\n", pvd->logical_block_size_le);

    k_printf("Volume Set Size: %u\n", pvd->vol_set_size_le);
    k_printf("Volume Sequence Number: %u\n", pvd->vol_seq_num_le);
    k_printf("Path Table Size: %u bytes\n", pvd->path_table_size_le);

    k_printf("L Path Table Location: 0x%08X\n", pvd->l_path_table_loc);
    k_printf("Optional L Path Table Location: 0x%08X\n",
             pvd->opt_l_path_table_loc);
    k_printf("M Path Table Location: 0x%08X\n", pvd->m_path_table_loc);
    k_printf("Optional M Path Table Location: 0x%08X\n",
             pvd->opt_m_path_table_loc);

    k_printf("--- Root Directory Record ---\n");
    const struct iso9660_dir_record *root = &pvd->root_dir_record;
    k_printf("  Length: %u\n", root->length);
    k_printf("  Extent (LBA): %u\n", root->extent_lba_le);
    k_printf("  Data Length: %u bytes\n", root->size_le);
    k_printf("  Flags: 0x%02X (%s)\n", root->flags,
             (root->flags & 0x02) ? "Directory" : "File");
}

enum errno iso9660_mount(struct generic_disk *disk) {
    struct iso9660_pvd pvd;
    if (iso9660_parse_pvd(disk, &pvd)) {
        return ERR_OK;
    }
    return ERR_FS_INTERNAL;
}

void iso9660_print(struct generic_disk *disk) {
    struct iso9660_pvd pvd;
    if (!iso9660_parse_pvd(disk, &pvd)) {
        return;
    }

    iso9660_pvd_print(&pvd);

    uint32_t root_lba = pvd.root_dir_record.extent_lba_le;
    uint32_t root_size = pvd.root_dir_record.size_le;
    uint32_t block_size = pvd.logical_block_size_le;
    uint32_t num_blocks = (root_size + block_size - 1) / block_size;

    uint8_t *dir_data = kmalloc(num_blocks * block_size);
    if (!disk->read_sector(disk, root_lba, dir_data, num_blocks)) {
        k_printf("Failed to read root directory data\n");
        kfree(dir_data);
        return;
    }

    k_printf("--- Root Directory Contents ---\n");

    size_t offset = 0;
    while (offset < root_size) {
        struct iso9660_dir_record *rec =
            (struct iso9660_dir_record *) (dir_data + offset);

        if (rec->length == 0) {
            offset = ((offset / block_size) + 1) * block_size;
            continue;
        }

        if (rec->name_len == 1 && (rec->name[0] == 0 || rec->name[0] == 1)) {
            offset += rec->length;
            continue;
        }

        char name[256] = {0};
        memcpy(name, rec->name, rec->name_len);
        name[rec->name_len] = '\0';

        k_printf("  %s  (LBA: %u, Size: %u bytes, %s)\n", name,
                 rec->extent_lba_le, rec->size_le,
                 (rec->flags & 0x02) ? "Directory" : "File");

        offset += rec->length;
    }

    kfree(dir_data);
}
