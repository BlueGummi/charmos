#include <console/printf.h>
#include <devices/generic_disk.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <string.h>

bool ext2_block_ptr_read(struct ext2_fs *fs, uint32_t block_num, void *buf) {
    if (!fs || !buf)
        return false;

    uint32_t lba = block_num * fs->sectors_per_block;

    struct generic_partition *p = fs->partition;
    struct generic_disk *d = fs->drive;

    return d->read_sector(d, lba + p->start_lba, (uint8_t *) buf,
                          fs->sectors_per_block);
}

bool ext2_inode_read(struct ext2_fs *fs, uint32_t inode_idx,
                     struct ext2_inode *inode_out) {
    if (!fs || !inode_out || inode_idx == 0)
        return false;

    uint32_t inodes_per_group = fs->sblock->inodes_per_group;
    uint32_t inode_size = fs->sblock->inode_size;

    uint32_t group = ext2_get_inode_group(fs, inode_idx);

    uint32_t index_in_group = (inode_idx - 1) % inodes_per_group;

    struct ext2_group_desc *desc = &fs->group_desc[group];
    uint32_t inode_table_block = desc->inode_table;

    uint32_t offset_bytes = index_in_group * inode_size;
    uint32_t block_offset = offset_bytes / fs->block_size;
    uint32_t offset_in_block = offset_bytes % fs->block_size;

    uint32_t inode_block_num = inode_table_block + block_offset;

    uint8_t *buf = kmalloc(fs->block_size);

    if (!buf || inode_idx == 0 || inode_idx > fs->sblock->inodes_count)
        return false;

    if (!ext2_block_ptr_read(fs, inode_block_num, buf)) {
        kfree(buf);
        return false;
    }

    memcpy(inode_out, buf + offset_in_block, sizeof(struct ext2_inode));

    kfree(buf);
    return true;
}

bool ext2_block_ptr_write(struct ext2_fs *fs, uint32_t block_num,
                          const void *buf) {
    if (!fs || !buf)
        return false;

    uint32_t lba = block_num * fs->sectors_per_block;
    struct generic_partition *p = fs->partition;
    struct generic_disk *d = fs->drive;

    return d->write_sector(d, lba + p->start_lba, (const uint8_t *) buf,
                           fs->sectors_per_block);
}

bool ext2_inode_write(struct ext2_fs *fs, uint32_t inode_num,
                      const struct ext2_inode *inode) {
    uint32_t group = ext2_get_inode_group(fs, inode_num);
    uint32_t index = (inode_num - 1) % fs->inodes_per_group;
    uint32_t offset = index * fs->inode_size;

    uint32_t inode_table_block = fs->group_desc[group].inode_table;
    uint32_t block_size = fs->block_size;

    uint32_t block_offset = offset % block_size;
    uint32_t block_index = offset / block_size;

    uint8_t *block_buf = kmalloc(block_size);
    if (!block_buf)
        return false;

    uint32_t inode_block_num = (inode_table_block + block_index);

    if (!ext2_block_ptr_read(fs, inode_block_num, block_buf)) {
        kfree(block_buf);
        return false;
    }

    memcpy(block_buf + block_offset, inode, fs->inode_size);

    bool status = ext2_block_ptr_write(fs, inode_block_num, block_buf);
    kfree(block_buf);
    return status;
}
