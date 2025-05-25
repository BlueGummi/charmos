#include <disk.h>
#include <fs/ext2.h>
#include <print.h>
#include <printf.h>
#include <stdint.h>
#include <string.h>
bool block_read(uint32_t lba, uint8_t *buffer, uint32_t sector_count) {
    for (uint32_t i = 0; i < sector_count; ++i) {
        if (!ide_read_sector(lba + i, buffer + (i * 512))) {
            return false;
        }
    }
    return true;
}

bool ext2_mount(struct ext2_fs *fs, struct ext2_sblock *sblock) {
    fs->sblock = sblock;
    fs->block_size = 1024 << sblock->log_block_size;
    fs->sectors_per_block = fs->block_size / 512;

    uint32_t gdt_block = (fs->block_size == 1024) ? 2 : 1;
    uint8_t gdt_buf[fs->block_size];

    if (!block_read(gdt_block * fs->sectors_per_block, gdt_buf,
                    fs->sectors_per_block)) {
        k_printf("Failed to read GDT\n");
        return false;
    }

    memcpy(&fs->group_desc, gdt_buf, sizeof(struct ext2_group_desc));
    return true;
}

bool ext2_read_inode(struct ext2_fs *fs, uint32_t inode_idx,
                     struct ext2_inode *inode_out) {
    uint32_t inode_table_block = fs->group_desc.inode_table;
    uint32_t inode_size = fs->sblock->inode_size;

    uint32_t offset_bytes = (inode_idx - 1) * inode_size;
    uint32_t block_offset = offset_bytes / fs->block_size;
    uint32_t offset_in_block = offset_bytes % fs->block_size;

    uint32_t inode_block_num = inode_table_block + block_offset;
    uint32_t inode_lba = inode_block_num * fs->sectors_per_block;

    uint8_t inode_buf[fs->block_size];
    if (!block_read(inode_lba, inode_buf, fs->sectors_per_block)) {
        k_printf("Failed to read inode block\n");
        return false;
    }

    memcpy(inode_out, inode_buf + offset_in_block, sizeof(struct ext2_inode));
    return true;
}

void ext2_list_dir(struct ext2_fs *fs, const struct ext2_inode *dir_inode) {
    if ((dir_inode->mode & 0xF000) != 0x4000) {
        k_printf("Error: inode is not a directory (mode=0x%x)\n",
                 dir_inode->mode);
        return;
    }

    for (uint32_t i = 0; i < 12; ++i) {
        if (dir_inode->block[i] == 0)
            continue;

        uint32_t dir_block_num = dir_inode->block[i];
        uint32_t dir_lba = dir_block_num * fs->sectors_per_block;
        uint8_t dir_buf[fs->block_size];

        if (!block_read(dir_lba, dir_buf, fs->sectors_per_block)) {
            k_printf("Failed to read directory block\n");
            continue;
        }

        uint32_t offset = 0;
        while (offset < fs->block_size) {
            struct ext2_dir_entry *entry =
                (struct ext2_dir_entry *) (dir_buf + offset);
            if (entry->rec_len == 0 || offset + entry->rec_len > fs->block_size)
                break;

            if (entry->inode != 0) {
                uint8_t name_len = entry->name_len;
                char name[256];
                memcpy(name, entry->name, name_len);
                name[name_len] = '\0';

                k_printf("inode=%u rec_len=%u name_len=%u name='%s'\n",
                         entry->inode, entry->rec_len, name_len, name);
            }

            offset += entry->rec_len;
        }
    }
}

void print_ext2_sblock(struct ext2_sblock *sblock) {
    k_printf("Inodes Count: %u\n", sblock->inodes_count);
    k_printf("Blocks Count: %u\n", sblock->blocks_count);
    k_printf("Reserved Blocks Count: %u\n", sblock->r_blocks_count);
    k_printf("Free Blocks Count: %u\n", sblock->free_blocks_count);
    k_printf("Free Inodes Count: %u\n", sblock->free_inodes_count);
    k_printf("First Data Block: %u\n", sblock->first_data_block);
    k_printf("Log Block Size: %u\n", sblock->log_block_size);
    k_printf("Log Fragment Size: %u\n", sblock->log_frag_size);
    k_printf("Blocks per Group: %u\n", sblock->blocks_per_group);
    k_printf("Fragments per Group: %u\n", sblock->frags_per_group);
    k_printf("Inodes per Group: %u\n", sblock->inodes_per_group);
    k_printf("Mount Time: ");
    ptime(sblock->mtime);
    k_printf("Write Time: ");
    ptime(sblock->wtime);
    k_printf("Mount Count: %u\n", sblock->mnt_count);
    k_printf("Max Mount Count: %u\n", sblock->max_mnt_count);
    k_printf("Magic: 0x%04x\n", sblock->magic);
    k_printf("State: %s\n", sblock->state == 1 ? "OK" : "ERROR");
    char *error_recov = "UNKNOWN";
    switch (sblock->errors) {
    case 1: error_recov = "IGNORE"; break;
    case 2: error_recov = "REMOUNT AS RO"; break;
    case 3: error_recov = "PANIC"; break;
    }
    k_printf("Error state: %s\n", error_recov);
    k_printf("Minor Revision Level: %u\n", sblock->minor_rev_level);
    k_printf("Last Check: ");
    ptime(sblock->lastcheck);
    k_printf("Check Interval: %u\n", sblock->checkinterval);
    char *creator = "OTHER";
    switch (sblock->creator_os) {
    case 0: creator = "LINUX"; break;
    case 1: creator = "GNU HURD"; break;
    case 2: creator = "MASIX"; break;
    case 3: creator = "FREEBSD"; break;
    }
    k_printf("Creator OS: %s\n", creator);
    k_printf("Revision Level: %u\n", sblock->rev_level);
    k_printf("Default Reserved UID: %u\n", sblock->def_resuid);
    k_printf("Default Reserved GID: %u\n", sblock->def_resgid);
    if (sblock->rev_level >= 1) {
        k_printf("First Inode: %u\n", sblock->first_ino);
        k_printf("Inode Size: %u\n", sblock->inode_size);
        k_printf("Block Group Number: %u\n", sblock->block_group_nr);
        k_printf("Feature Compatibility: %u\n", sblock->feature_compat);
        k_printf("Feature Incompatibility: %u\n", sblock->feature_incompat);
        k_printf("Feature Read-Only Compatibility: %u\n",
                 sblock->feature_ro_compat);

        k_printf("UUID: ");
        for (int i = 0; i < 16; i++) {
            k_printf("%02x", sblock->uuid[i]);
            if (i < 15)
                k_printf("-");
        }
        k_printf("\n");

        k_printf("Volume Name: %-16s\n", sblock->volume_name);
        k_printf("Last Mounted: %-64s\n", sblock->last_mounted);
        k_printf("Algorithm Usage Bitmap: %u\n",
                 sblock->algorithm_usage_bitmap);
        k_printf("Preallocated Blocks: %u\n", sblock->prealloc_blocks);
        k_printf("Preallocated Directory Blocks: %u\n",
                 sblock->prealloc_dir_blocks);
        k_printf("Padding: %u\n", sblock->padding);
    }
    struct ext2_fs fs;
    if (!ext2_mount(&fs, sblock)) {
        k_printf("Mount failed\n");
        return;
    }

    struct ext2_inode root_inode;
    if (!ext2_read_inode(&fs, EXT2_ROOT_INODE, &root_inode)) {
        k_printf("Failed to read root inode\n");
        return;
    }

    ext2_list_dir(&fs, &root_inode);
}
