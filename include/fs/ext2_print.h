#include <devices/generic_disk.h>
#include <fs/ext2.h>

void ext2_print_superblock(struct ext2_sblock *sblock);

void ext2_print_inode(const struct ext2_full_inode *node);

void ext2_dump_file_data(struct ext2_fs *fs, const struct ext2_inode *inode,
                         uint32_t start_block_index, uint32_t length);

#pragma once
