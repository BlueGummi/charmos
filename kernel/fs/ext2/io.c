#include <block/generic.h>
#include <fs/ext2.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

uint32_t ext2_block_to_lba(struct ext2_fs *fs, uint32_t block_num) {
    if (!fs)
        return -1;

    struct generic_partition *p = fs->partition;

    uint32_t base_lba = block_num * fs->sectors_per_block;
    uint32_t lba = base_lba + p->start_lba;
    return lba;
}

uint8_t *ext2_block_read(struct ext2_fs *fs, uint32_t block_num,
                         struct bcache_entry **out) {
    if (!fs)
        return NULL;

    struct generic_disk *d = fs->drive;

    uint32_t lba = ext2_block_to_lba(fs, block_num);
    uint32_t spb = fs->sectors_per_block;

    uint8_t *buf = bcache_get(d, lba, fs->block_size, spb, false, out);

    bcache_ent_acquire(*out);
    return buf;
}

bool ext2_block_write(struct ext2_fs *fs, struct bcache_entry *ent,
                      enum bio_request_priority prio) {
    if (!fs || !ent)
        return false;

    struct generic_disk *d = fs->drive;

    bcache_write_queue(d, ent, fs->sectors_per_block, prio);

    return true;
}

struct ext2_inode *ext2_inode_read(struct ext2_fs *fs, uint32_t inode_idx,
                                   struct bcache_entry **out_ent) {
    if (!fs || inode_idx == 0 || inode_idx > fs->inodes_count) {
        return NULL;
    }

    uint32_t inodes_per_group = fs->inodes_per_group;
    uint32_t inode_size = fs->inode_size;

    uint32_t group = ext2_get_inode_group(fs, inode_idx);
    uint32_t index_in_group = (inode_idx - 1) % inodes_per_group;

    struct ext2_group_desc *desc = &fs->group_desc[group];
    uint32_t inode_table_block = desc->inode_table;

    uint32_t offset_bytes = index_in_group * inode_size;
    uint32_t block_offset = offset_bytes / fs->block_size;
    uint32_t offset_in_block = offset_bytes % fs->block_size;

    uint32_t inode_block_num = inode_table_block + block_offset;

    struct bcache_entry *ent;
    uint8_t *buf = ext2_block_read(fs, inode_block_num, &ent);
    if (!buf)
        return NULL;

    if (out_ent)
        *out_ent = ent;

    struct ext2_inode *inode_ptr =
        (struct ext2_inode *) (buf + offset_in_block);
    return inode_ptr;
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

    uint32_t inode_block_num = (inode_table_block + block_index);
    struct bcache_entry *ent;
    uint8_t *block_buf = ext2_block_read(fs, inode_block_num, &ent);
    if (!block_buf)
        return false;

    memcpy(block_buf + block_offset, inode, fs->inode_size);
    bcache_ent_release(ent);

    bool status = ext2_block_write(fs, ent, EXT2_PRIO_INODE);
    return status;
}
