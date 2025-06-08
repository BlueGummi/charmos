#include <console/printf.h>
#include <fs/fat.h>
#include <mem/alloc.h>
#include <string.h>

#define create_str(new_str, orig_str, orig_str_size)                           \
    char new_str[orig_str_size + 1] = {0};                                     \
    memcpy(new_str, orig_str, orig_str_size);                                  \
    new_str[orig_str_size] = '\0';                                             \
    for (int i = orig_str_size - 1; i >= 0; i--) {                             \
        if (new_str[i] == ' ')                                                 \
            new_str[i] = '\0';                                                 \
        else                                                                   \
            break;                                                             \
    }

void fat32_print_bpb(const struct fat_bpb *bpb) {
    create_str(oem_name, bpb->oem_name, 8);
    create_str(volume_label, bpb->ext_32.volume_label, 11);
    create_str(fs_type, bpb->ext_32.fs_type, 8);

    k_printf("FAT32 BPB:\n");
    k_printf("  OEM Name: %s\n", oem_name);
    k_printf("  Bytes/Sector: %u\n", bpb->bytes_per_sector);
    k_printf("  Sectors/Cluster: %u\n", bpb->sectors_per_cluster);
    k_printf("  Reserved Sectors: %u\n", bpb->reserved_sector_count);
    k_printf("  Number of FATs: %u\n", bpb->num_fats);
    k_printf("  Total Sectors: %u\n", bpb->total_sectors_32);
    k_printf("  FAT Size (sectors): %u\n", bpb->ext_32.fat_size_32);
    k_printf("  Root Cluster: %u\n", bpb->ext_32.root_cluster);
    k_printf("  Volume Label: %s\n", volume_label);
    k_printf("  FS Type: %s\n", fs_type);
}

void fat_print_dirent(const struct fat_dirent *ent) {
    if ((uint8_t) ent->name[0] == 0xE5)
        return;

    if ((uint8_t) ent->name[0] == 0x00)
        return;

    if ((ent->attr & 0x0F) == 0x0F)
        return;

    create_str(name, ent->name, 11);

    uint32_t cluster = ((uint32_t) ent->high_cluster << 16) | ent->low_cluster;

    k_printf("Dirent: %-11s | Attr: %-13s (0x%02x) | Cluster: %u | Size: %u\n",
             name, get_fileattr_string(ent->attr), ent->attr, cluster,
             ent->filesize);
}

void fat32_list_root(struct generic_disk *disk) {
    struct fat_fs *fs = disk->fs_data;
    if (!fs || !fs->bpb)
        return;

    uint32_t cluster_size =
        fs->bpb->sectors_per_cluster * fs->bpb->bytes_per_sector;
    uint8_t *cluster_buf = kmalloc(cluster_size);
    if (!cluster_buf)
        return;

    uint32_t cluster = fs->bpb->ext_32.root_cluster;

    if (fat32_read_cluster(disk, cluster, cluster_buf)) {
        for (uint32_t i = 0; i < cluster_size; i += sizeof(struct fat_dirent)) {
            struct fat_dirent *entry = (struct fat_dirent *) (cluster_buf + i);
            fat_print_dirent(entry);
        }
    }

    kfree(cluster_buf);
}
