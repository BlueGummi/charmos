#include <console/printf.h>
#include <fs/ext2.h>
#include <time/print.h>

void ext2_print_inode(const struct ext2_full_inode *node) {
    if (!node)
        return;

    struct ext2_inode inode = node->node;
    k_printf("Inode Information:\n");
    k_printf("  Block number: %u\n", node->inode_num);
    k_printf("  Mode: 0x%04x\n", inode.mode);
    k_printf("  UID: %u\n", inode.uid);
    k_printf("  GID: %u\n", inode.gid);
    k_printf("  Size: %u bytes\n", inode.size);
    k_printf("  Access Time: ");
    time_print_unix(inode.atime);
    k_printf("  Creation Time: ");
    time_print_unix(inode.ctime);
    k_printf("  Modification Time: ");
    time_print_unix(inode.mtime);
    k_printf("  Deletion Time: ");
    time_print_unix(inode.dtime);
    k_printf("  Links Count: %u\n", inode.links_count);
    k_printf("  Blocks: %u\n", inode.blocks);
    k_printf("  Flags: 0x%08x\n", inode.flags);
    k_printf("  osd1: 0x%08x\n", inode.osd1);

    k_printf("  Block Pointers:\n");
    for (int i = 0; i < EXT2_NBLOCKS; ++i) {
        k_printf("    [%-2d] = %u\n", i, inode.block[i]);
    }

    k_printf("  Generation: %u\n", inode.generation);
    k_printf("  File ACL: %u\n", inode.file_acl);
    k_printf("  Directory ACL: %u\n", inode.dir_acl);
    k_printf("  Fragment Address: %u\n", inode.faddr);

    k_printf("  Fragment (raw): ");
    for (int i = 0; i < 16; ++i) {
        k_printf("%02x ", inode.frag[i]);
    }
    k_printf("\n");

    k_printf("  OS Specific 2: ");
    for (int i = 0; i < 12; ++i) {
        k_printf("%02x ", inode.osd2[i]);
    }
    k_printf("\n");
}

void ext2_print_superblock(struct ext2_sblock *sblock) {
    if (!sblock)
        return;

    k_printf("Ext2 superblock information:\n");
    k_printf("  Inodes Count: %u\n", sblock->inodes_count);
    k_printf("  Blocks Count: %u\n", sblock->blocks_count);
    k_printf("  Reserved Blocks Count: %u\n", sblock->r_blocks_count);
    k_printf("  Free Blocks Count: %u\n", sblock->free_blocks_count);
    k_printf("  Free Inodes Count: %u\n", sblock->free_inodes_count);
    k_printf("  First Data Block: %u\n", sblock->first_data_block);
    k_printf("  Log Block Size: %u\n", sblock->log_block_size);
    k_printf("  Log Fragment Size: %u\n", sblock->log_frag_size);
    k_printf("  Blocks per Group: %u\n", sblock->blocks_per_group);
    k_printf("  Fragments per Group: %u\n", sblock->frags_per_group);
    k_printf("  Inodes per Group: %u\n", sblock->inodes_per_group);
    k_printf("  Mount Time: ");
    time_print_unix(sblock->mtime);
    k_printf("  Write Time: ");
    time_print_unix(sblock->wtime);
    k_printf("  Mount Count: %u\n", sblock->mnt_count);
    k_printf("  Max Mount Count: %u\n", sblock->max_mnt_count);
    k_printf("  Magic: 0x%04x\n", sblock->magic);
    k_printf("  State: %s\n", sblock->state == 1 ? "OK" : "ERROR");
    char *error_recov = "UNKNOWN";
    switch (sblock->errors) {
    case 1: error_recov = "IGNORE"; break;
    case 2: error_recov = "REMOUNT AS RO"; break;
    case 3: error_recov = "PANIC"; break;
    }
    k_printf("  Error state: %s\n", error_recov);
    k_printf("  Minor Revision Level: %u\n", sblock->minor_rev_level);
    k_printf("  Last Check: ");
    time_print_unix(sblock->lastcheck);
    k_printf("  Check Interval: %u\n", sblock->checkinterval);
    char *creator = "OTHER";
    switch (sblock->creator_os) {
    case 0: creator = "LINUX"; break;
    case 1: creator = "GNU HURD"; break;
    case 2: creator = "MASIX"; break;
    case 3: creator = "FREEBSD"; break;
    }
    k_printf("  Creator OS: %s\n", creator);
    k_printf("  Revision Level: %u\n", sblock->rev_level);
    k_printf("  Default Reserved UID: %u\n", sblock->def_resuid);
    k_printf("  Default Reserved GID: %u\n", sblock->def_resgid);
    if (sblock->rev_level >= 1) {
        k_printf("  First Inode: %u\n", sblock->first_ino);
        k_printf("  Inode Size: %u\n", sblock->inode_size);
        k_printf("  Block Group Number: %u\n", sblock->block_group_nr);
        k_printf("  Feature Compatibility: %u\n", sblock->feature_compat);
        k_printf("  Feature Incompatibility: %u\n", sblock->feature_incompat);
        k_printf("  Feature Read-Only Compatibility: %u\n",
                 sblock->feature_ro_compat);

        k_printf("  UUID: ");
        for (int i = 0; i < 16; i++) {
            k_printf("%02x", sblock->uuid[i]);
            if (i < 15)
                k_printf("-");
        }
        k_printf("\n");

        k_printf("  Volume Name: %-16s\n", sblock->volume_name);
        k_printf("  Last Mounted: %-64s\n", sblock->last_mounted);
        k_printf("  Algorithm Usage Bitmap: %u\n",
                 sblock->algorithm_usage_bitmap);
        k_printf("  Preallocated Blocks: %u\n", sblock->prealloc_blocks);
        k_printf("  Preallocated Directory Blocks: %u\n",
                 sblock->prealloc_dir_blocks);
        k_printf("  Padding: %u\n", sblock->padding);
    }
}
