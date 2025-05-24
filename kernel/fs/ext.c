#include <ext2.h>
#include <printf.h>

void print_ext2_sblock(const struct ext2_sblock *sblock) {
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
    k_printf("Mount Time: %u\n", sblock->mtime);
    k_printf("Write Time: %u\n", sblock->wtime);
    k_printf("Mount Count: %u\n", sblock->mnt_count);
    k_printf("Max Mount Count: %u\n", sblock->max_mnt_count);
    k_printf("Magic: 0x%04X\n", sblock->magic);
    k_printf("State: %u\n", sblock->state);
    k_printf("Errors: %u\n", sblock->errors);
    k_printf("Minor Revision Level: %u\n", sblock->minor_rev_level);
    k_printf("Last Check: %u\n", sblock->lastcheck);
    k_printf("Check Interval: %u\n", sblock->checkinterval);
    k_printf("Creator OS: %u\n", sblock->creator_os);
    k_printf("Revision Level: %u\n", sblock->rev_level);
    k_printf("Default Reserved UID: %u\n", sblock->def_resuid);
    k_printf("Default Reserved GID: %u\n", sblock->def_resgid);
    k_printf("First Inode: %u\n", sblock->first_ino);
    k_printf("Inode Size: %u\n", sblock->inode_size);
    k_printf("Block Group Number: %u\n", sblock->block_group_nr);
    k_printf("Feature Compatibility: %u\n", sblock->feature_compat);
    k_printf("Feature Incompatibility: %u\n", sblock->feature_incompat);
    k_printf("Feature Read-Only Compatibility: %u\n", sblock->feature_ro_compat);
    
    k_printf("UUID: ");
    for (int i = 0; i < 16; i++) {
        k_printf("%02x", sblock->uuid[i]);
        if (i < 15) k_printf("-");
    }
    k_printf("\n");

    k_printf("Volume Name: %.16s\n", sblock->volume_name);
    k_printf("Last Mounted: %.64s\n", sblock->last_mounted);
    k_printf("Algorithm Usage Bitmap: %u\n", sblock->algorithm_usage_bitmap);
    k_printf("Preallocated Blocks: %u\n", sblock->prealloc_blocks);
    k_printf("Preallocated Directory Blocks: %u\n", sblock->prealloc_dir_blocks);
    k_printf("Padding: %u\n", sblock->padding);
}
