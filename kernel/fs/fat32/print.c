#include <console/printf.h>
#include <fs/fat32.h>
#include <string.h>

void fat32_print_bpb(const struct fat32_bpb *bpb) {
    k_printf("FAT32 BPB:\n");
    k_printf("  OEM Name: %-8s\n", bpb->oem_name);
    k_printf("  Bytes/Sector: %u\n", bpb->bytes_per_sector);
    k_printf("  Sectors/Cluster: %u\n", bpb->sectors_per_cluster);
    k_printf("  Reserved Sectors: %u\n", bpb->reserved_sector_count);
    k_printf("  Number of FATs: %u\n", bpb->num_fats);
    k_printf("  Total Sectors: %u\n", bpb->total_sectors_32);
    k_printf("  FAT Size (sectors): %u\n", bpb->fat_size_32);
    k_printf("  Root Cluster: %u\n", bpb->root_cluster);
    k_printf("  Volume Label: %-11s\n", bpb->volume_label);
    k_printf("  FS Type: %-8s\n", bpb->fs_type);
}

void fat32_print_dirent(const struct fat_dirent *ent) {
    if ((uint8_t) ent->name[0] == 0xE5)
        return;
    if ((uint8_t) ent->name[0] == 0x00)
        return;
    if ((ent->attr & 0x0F) == 0x0F)
        return;

    char name[12] = {0};
    memcpy(name, ent->name, 11);

    for (int i = 10; i >= 0; i--) {
        if (name[i] == ' ' || name[i] == 0)
            name[i] = 0;
        else
            break;
    }

    uint32_t cluster = ((uint32_t) ent->high_cluster << 16) | ent->low_cluster;

    k_printf("Dirent: %-11s | Attr: 0x%02x | Cluster: %u | Size: %u\n", name,
             ent->attr, cluster, ent->filesize);
}
