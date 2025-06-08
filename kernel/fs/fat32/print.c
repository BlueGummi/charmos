#include <console/printf.h>
#include <fs/fat32.h>
#include <string.h>

void fat32_print_bpb(const struct fat_bpb *bpb) {
    char oem_name[9];
    memcpy(oem_name, bpb->oem_name, 8);
    oem_name[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        if (oem_name[i] == ' ')
            oem_name[i] = '\0';
        else
            break;
    }

    char volume_label[12];
    memcpy(volume_label, bpb->ext_32.volume_label, 11);
    volume_label[11] = '\0';
    for (int i = 10; i >= 0; i--) {
        if (volume_label[i] == ' ')
            volume_label[i] = '\0';
        else
            break;
    }

    char fs_type[9];
    memcpy(fs_type, bpb->ext_32.fs_type, 8);
    fs_type[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        if (fs_type[i] == ' ')
            fs_type[i] = '\0';
        else
            break;
    }

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
