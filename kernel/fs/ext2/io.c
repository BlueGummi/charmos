#include <mem/alloc.h>
#include <disk/generic_disk.h>
#include <fs/ext2.h>
#include <stdint.h>
#include <string.h>

bool ext2_block_read(struct generic_disk *d, uint32_t lba, uint8_t *buffer,
                     uint32_t sector_count) {
    if (!buffer)
        return false;

    for (uint32_t i = 0; i < sector_count; ++i) {
        if (!d->read_sector(d, lba + i, buffer + (i * d->sector_size))) {
            return false;
        }
    }
    return true;
}

bool ext2_block_ptr_read(struct ext2_fs *fs, uint32_t block_num, void *buf) {
    if (!fs || !buf)
        return false;

    uint32_t lba = block_num * fs->sectors_per_block;
    return ext2_block_read(fs->drive, lba, (uint8_t *) buf,
                           fs->sectors_per_block);
}

bool ext2_read_inode(struct ext2_fs *fs, uint32_t inode_idx,
                     struct ext2_inode *inode_out) {
    if (!fs || !inode_out || inode_idx == 0)
        return false;

    uint32_t inodes_per_group = fs->sblock->inodes_per_group;
    uint32_t inode_size = fs->sblock->inode_size;

    uint32_t group = (inode_idx - 1) / inodes_per_group;
    uint32_t index_in_group = (inode_idx - 1) % inodes_per_group;

    struct ext2_group_desc *desc = &fs->group_desc[group];
    uint32_t inode_table_block = desc->inode_table;

    uint32_t offset_bytes = index_in_group * inode_size;
    uint32_t block_offset = offset_bytes / fs->block_size;
    uint32_t offset_in_block = offset_bytes % fs->block_size;

    uint32_t inode_block_num = inode_table_block + block_offset;
    uint32_t inode_lba = inode_block_num * fs->sectors_per_block;

    uint8_t *buf = kmalloc(fs->block_size);

    if (!buf || inode_idx == 0 || inode_idx > fs->sblock->inodes_count)
        return false;

    if (!ext2_block_read(fs->drive, inode_lba, buf, fs->sectors_per_block)) {
        kfree(buf);
        return false;
    }

    memcpy(inode_out, buf + offset_in_block, sizeof(struct ext2_inode));
    kfree(buf);
    return true;
}

bool ext2_block_write(struct generic_disk *d, uint32_t lba,
                      const uint8_t *buffer, uint32_t sector_count) {
    if (!buffer)
        return false;

    for (uint32_t i = 0; i < sector_count; ++i) {
        if (!d->write_sector(d, lba + i, buffer + (i * d->sector_size))) {
            return false;
        }
    }
    return true;
}

bool ext2_block_ptr_write(struct ext2_fs *fs, uint32_t block_num, void *buf) {
    if (!fs || !buf)
        return false;

    uint32_t lba = block_num * fs->sectors_per_block;
    return ext2_block_write(fs->drive, lba, (const uint8_t *) buf,
                            fs->sectors_per_block);
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
    if (!ext2_block_read(fs->drive, lba, block_buf, fs->sectors_per_block))
        return false;

    memcpy(block_buf + block_offset, inode, fs->inode_size);

    return ext2_block_write(fs->drive, lba, block_buf, fs->sectors_per_block);
}
