#include <disk.h>
#include <fs/ext2.h>
#include <string.h>

bool block_write(struct ide_drive *d, uint32_t lba, const uint8_t *buffer, uint32_t sector_count) {
    if (!buffer)
        return false;

    for (uint32_t i = 0; i < sector_count; ++i) {
        if (!ide_write_sector(d, lba + i, buffer + (i * 512))) {
            return false;
        }
    }
    return true;
}


bool ext2_write_inode(struct ext2_fs *fs, uint32_t inode_num,
                      const struct ext2_inode *inode) {
    uint32_t group = (inode_num - 1) / fs->inodes_per_group;
    uint32_t index = (inode_num - 1) % fs->inodes_per_group;
    uint32_t offset = index * fs->inode_size;

    uint32_t inode_table_block = fs->group_desc[group].inode_table;
    uint32_t block_size = fs->block_size;

    uint32_t block_offset = offset % block_size;
    uint32_t block_index = offset / block_size;

    uint8_t block_buf[block_size];

    uint32_t lba = (inode_table_block + block_index) * fs->sectors_per_block;
    if (!block_read(fs->drive, lba, block_buf, fs->sectors_per_block))
        return false;

    memcpy(block_buf + block_offset, inode, fs->inode_size);

    return block_write(fs->drive, lba, block_buf, fs->sectors_per_block);
}
