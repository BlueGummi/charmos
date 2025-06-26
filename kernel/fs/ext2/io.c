#include <console/printf.h>
#include <devices/generic_disk.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
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

/* these should now return block cache entries */
struct block_cache_entry *ext2_block_read(struct ext2_fs *fs,
                                          uint32_t block_num) {
    if (!fs)
        return NULL;

    struct generic_disk *d = fs->drive;

    uint32_t lba = ext2_block_to_lba(fs, block_num);
    uint32_t spb = fs->sectors_per_block;

    return bcache_get(d, lba, fs->block_size, spb, false);
}

bool ext2_block_write(struct ext2_fs *fs, struct block_cache_entry *ent) {
    if (!fs || !ent)
        return false;

    if (!ent->buffer)
        return false;

    struct generic_disk *d = fs->drive;

    const uint8_t *buf = (const uint8_t *) ent->buffer;

    uint32_t spb = fs->sectors_per_block;
    
    /* this updates access times */
    bcache_get(d, ent->lba, fs->block_size, spb, false);

    return d->write_sector(d, ent->lba, buf, fs->sectors_per_block);
}

struct block_cache_entry *ext2_inode_read(struct ext2_fs *fs,
                                          uint32_t inode_idx,
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

    if (inode_idx == 0 || inode_idx > fs->sblock->inodes_count)
        return false;

    struct block_cache_entry *ent = ext2_block_read(fs, inode_block_num);
    if (!ent)
        return NULL;

    uint8_t *buf = ent->buffer;

    memcpy(inode_out, buf + offset_in_block, sizeof(struct ext2_inode));

    return ent;
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
    struct block_cache_entry *ent = ext2_block_read(fs, inode_block_num);
    if (!ent)
        return false;

    uint8_t *block_buf = ent->buffer;

    memcpy(block_buf + block_offset, inode, fs->inode_size);

    bool status = ext2_block_write(fs, ent);
    return status;
}
