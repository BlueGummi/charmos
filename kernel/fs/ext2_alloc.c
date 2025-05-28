#include <alloc.h>
#include <fs/ext2.h>
#include <printf.h>
#include <stdint.h>

static bool find_free_bit(uint8_t *bitmap, uint32_t size, uint32_t *byte_pos,
                          uint32_t *bit_pos) {
    for (*byte_pos = 0; *byte_pos < size; ++(*byte_pos)) {
        if (bitmap[*byte_pos] != 0xFF) {
            for (*bit_pos = 0; *bit_pos < 8; ++(*bit_pos)) {
                if (!(bitmap[*byte_pos] & (1 << *bit_pos))) {
                    return true;
                }
            }
        }
    }
    return false;
}

static uint32_t alloc_from_bitmap(struct ext2_fs *fs, uint32_t bitmap_block,
                                  uint32_t items_per_group, uint32_t group,
                                  void (*update_counts)(struct ext2_fs *,
                                                        uint32_t)) {
    uint8_t bitmap[fs->block_size];
    uint32_t lba = bitmap_block * fs->sectors_per_block;
    uint32_t byte_pos, bit_pos;

    if (!block_read(fs->drive, lba, bitmap, fs->sectors_per_block))
        return -1;

    if (!find_free_bit(bitmap, fs->block_size, &byte_pos, &bit_pos))
        return -1;

    bitmap[byte_pos] |= (1 << bit_pos);
    if (!block_write(fs->drive, lba, bitmap, fs->sectors_per_block))
        return -1;

    update_counts(fs, group);

    return group * items_per_group + (byte_pos * 8 + bit_pos);
}

static void update_block_counts(struct ext2_fs *fs, uint32_t group) {
    fs->group_desc[group].free_blocks_count--;
    fs->sblock->free_blocks_count--;
    ext2_write_group_desc(fs);
    ext2_write_superblock(fs);
}

static void update_inode_counts(struct ext2_fs *fs, uint32_t group) {
    fs->group_desc[group].free_inodes_count--;
    fs->sblock->free_inodes_count--;
    ext2_write_group_desc(fs);
    ext2_write_superblock(fs);
}

uint32_t ext2_alloc_block(struct ext2_fs *fs) {
    if (!fs)
        return -1;

    for (uint32_t group = 0; group < fs->num_groups; ++group) {
        uint32_t block_num = alloc_from_bitmap(
            fs, fs->group_desc[group].block_bitmap,
            fs->sblock->blocks_per_group, group, update_block_counts);
        if (block_num != (uint32_t) -1) {
            return block_num;
        }
    }
    return -1;
}

bool ext2_free_block(struct ext2_fs *fs, uint32_t block_num) {
    if (!fs || block_num == 0)
        return false;

    uint32_t group = block_num / fs->sblock->blocks_per_group;
    uint32_t index = block_num % fs->sblock->blocks_per_group;

    uint32_t bitmap_block = fs->group_desc[group].block_bitmap;

    uint8_t *bitmap = kmalloc(fs->block_size);
    if (!bitmap)
        return false;

    block_read(fs->drive, bitmap_block, bitmap, fs->sectors_per_block);

    uint32_t byte = index / 8;
    uint8_t bit = 1 << (index % 8);
    if (!(bitmap[byte] & bit)) {
        k_printf("Block %u already free\n", block_num);
        return false;
    }

    bitmap[byte] &= ~bit;
    block_write(fs->drive, bitmap_block, bitmap, fs->sectors_per_block);

    kfree(bitmap);

    uint8_t *zero_buf = kzalloc(fs->block_size);
    if (zero_buf) {
        block_write(fs->drive, block_num * fs->sectors_per_block, zero_buf,
                    fs->sectors_per_block);
        kfree(zero_buf);
    }

    fs->group_desc[group].free_blocks_count++;
    fs->sblock->free_blocks_count++;

    ext2_write_group_desc(fs);
    ext2_write_superblock(fs);
    return true;
}

uint32_t ext2_alloc_inode(struct ext2_fs *fs) {
    if (!fs)
        return -1;

    for (uint32_t group = 0; group < fs->num_groups; ++group) {
        uint32_t inode_num =
            alloc_from_bitmap(fs, fs->group_desc[group].inode_bitmap,
                              fs->inodes_per_group, group, update_inode_counts);
        if (inode_num != (uint32_t) -1) {
            return inode_num + 1;
        }
    }
    return -1;
}

bool ext2_free_inode(struct ext2_fs *fs, uint32_t inode_num) {
    if (!fs || inode_num == 0)
        return false;

    inode_num -= 1;
    uint32_t group = inode_num / fs->inodes_per_group;
    uint32_t index = inode_num % fs->inodes_per_group;

    uint32_t bitmap_block = fs->group_desc[group].inode_bitmap;
    uint8_t *bitmap = kmalloc(fs->block_size);

    block_read(fs->drive, bitmap_block, bitmap, fs->sectors_per_block);
    if (!bitmap)
        return false;

    uint32_t byte = index / 8;
    uint8_t bit = 1 << (index % 8);
    if (!(bitmap[byte] & bit)) {
        k_printf("Inode %u already free\n", inode_num + 1);
        return false;
    }

    bitmap[byte] &= ~bit;
    block_write(fs->drive, bitmap_block, bitmap, fs->sectors_per_block);
    kfree(bitmap);

    fs->group_desc[group].free_inodes_count++;
    fs->sblock->free_inodes_count++;
    ext2_write_group_desc(fs);
    ext2_write_superblock(fs);

    struct ext2_inode empty = {0};
    ext2_write_inode(fs, inode_num + 1, &empty);

    return true;
}
