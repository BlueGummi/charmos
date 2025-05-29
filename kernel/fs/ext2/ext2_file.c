#include <alloc.h>
#include <fs/ext2.h>
#include <printf.h>
#include <string.h>

static uint32_t get_or_set_block(struct ext2_fs *fs, struct ext2_inode *inode,
                                 uint32_t block_index, uint32_t new_block_num,
                                 bool allocate, bool *was_allocated) {
    if (!fs || !inode)
        return (uint32_t) -1;

    uint32_t pointers_per_block = fs->block_size / sizeof(uint32_t);

    // direct
    if (block_index < 12) {
        if (inode->block[block_index] == 0 && allocate) {
            if (new_block_num == 0) {
                new_block_num = ext2_alloc_block(fs);
                if (new_block_num == 0)
                    return 0;
            }
            inode->block[block_index] = new_block_num;
            *was_allocated = true;
        }
        return inode->block[block_index];
    }

    block_index -= 12;

    // single
    if (block_index < pointers_per_block) {
        if (inode->block[12] == 0) {
            if (!allocate)
                return 0;
            inode->block[12] = ext2_alloc_block(fs);
            if (inode->block[12] == 0)
                return 0;
        }

        uint32_t *indirect_block = kmalloc(fs->block_size);
        if (!indirect_block)
            return 0;

        block_ptr_read(fs, inode->block[12], (uint8_t *) indirect_block);

        uint32_t block_num = indirect_block[block_index];
        if (block_num == 0 && allocate) {
            if (new_block_num == 0) {
                new_block_num = ext2_alloc_block(fs);
                if (new_block_num == 0) {
                    kfree(indirect_block);
                    return 0;
                }
            }
            indirect_block[block_index] = new_block_num;
            *was_allocated = true;
            block_ptr_write(fs, inode->block[12], (uint8_t *) indirect_block);
            block_num = new_block_num;
        }

        kfree(indirect_block);
        return block_num;
    }

    block_index -= pointers_per_block;

    // double
    if (block_index < pointers_per_block * pointers_per_block) {
        if (inode->block[13] == 0) {
            if (!allocate)
                return 0;
            inode->block[13] = ext2_alloc_block(fs);
            if (inode->block[13] == 0)
                return 0;
        }

        uint32_t *double_indirect_block = kmalloc(fs->block_size);
        if (!double_indirect_block)
            return 0;

        block_ptr_read(fs, inode->block[13], (uint8_t *) double_indirect_block);

        uint32_t first_index = block_index / pointers_per_block;
        uint32_t second_index = block_index % pointers_per_block;

        if (double_indirect_block[first_index] == 0) {
            if (!allocate) {
                kfree(double_indirect_block);
                return 0;
            }
            double_indirect_block[first_index] = ext2_alloc_block(fs);
            if (double_indirect_block[first_index] == 0) {
                kfree(double_indirect_block);
                return 0;
            }
            block_ptr_write(fs, inode->block[13],
                            (uint8_t *) double_indirect_block);
        }

        uint32_t *single_indirect_block = kmalloc(fs->block_size);
        if (!single_indirect_block) {
            kfree(double_indirect_block);
            return 0;
        }

        block_ptr_read(fs, double_indirect_block[first_index],
                       (uint8_t *) single_indirect_block);

        uint32_t block_num = single_indirect_block[second_index];
        if (block_num == 0 && allocate) {
            if (new_block_num == 0) {
                new_block_num = ext2_alloc_block(fs);
                if (new_block_num == 0) {
                    kfree(single_indirect_block);
                    kfree(double_indirect_block);
                    return 0;
                }
            }
            single_indirect_block[second_index] = new_block_num;
            *was_allocated = true;
            block_ptr_write(fs, double_indirect_block[first_index],
                            (uint8_t *) single_indirect_block);
            block_num = new_block_num;
        }

        kfree(single_indirect_block);
        kfree(double_indirect_block);
        return block_num;
    }

    block_index -= pointers_per_block * pointers_per_block;

    // triple
    if (block_index < ((uint64_t) pointers_per_block * pointers_per_block *
                       pointers_per_block)) {
        if (inode->block[14] == 0) {
            if (!allocate)
                return 0;
            inode->block[14] = ext2_alloc_block(fs);
            if (inode->block[14] == 0)
                return 0;
        }

        uint32_t *triple_indirect_block = kmalloc(fs->block_size);
        if (!triple_indirect_block)
            return 0;

        block_ptr_read(fs, inode->block[14], (uint8_t *) triple_indirect_block);

        uint32_t first_index =
            block_index / (pointers_per_block * pointers_per_block);
        uint32_t remainder =
            block_index % (pointers_per_block * pointers_per_block);
        uint32_t second_index = remainder / pointers_per_block;
        uint32_t third_index = remainder % pointers_per_block;

        if (triple_indirect_block[first_index] == 0) {
            if (!allocate) {
                kfree(triple_indirect_block);
                return 0;
            }
            triple_indirect_block[first_index] = ext2_alloc_block(fs);
            if (triple_indirect_block[first_index] == 0) {
                kfree(triple_indirect_block);
                return 0;
            }
            block_ptr_write(fs, inode->block[14],
                            (uint8_t *) triple_indirect_block);
        }

        uint32_t *double_indirect_block = kmalloc(fs->block_size);
        if (!double_indirect_block) {
            kfree(triple_indirect_block);
            return 0;
        }

        block_ptr_read(fs, triple_indirect_block[first_index],
                       (uint8_t *) double_indirect_block);

        if (double_indirect_block[second_index] == 0) {
            if (!allocate) {
                kfree(double_indirect_block);
                kfree(triple_indirect_block);
                return 0;
            }
            double_indirect_block[second_index] = ext2_alloc_block(fs);
            if (double_indirect_block[second_index] == 0) {
                kfree(double_indirect_block);
                kfree(triple_indirect_block);
                return 0;
            }
            block_ptr_write(fs, triple_indirect_block[first_index],
                            (uint8_t *) double_indirect_block);
        }

        uint32_t *single_indirect_block = kmalloc(fs->block_size);
        if (!single_indirect_block) {
            kfree(double_indirect_block);
            kfree(triple_indirect_block);
            return 0;
        }

        block_ptr_read(fs, double_indirect_block[second_index],
                       (uint8_t *) single_indirect_block);

        uint32_t block_num = single_indirect_block[third_index];
        if (block_num == 0 && allocate) {
            if (new_block_num == 0) {
                new_block_num = ext2_alloc_block(fs);
                if (new_block_num == 0) {
                    kfree(single_indirect_block);
                    kfree(double_indirect_block);
                    kfree(triple_indirect_block);
                    return 0;
                }
            }
            single_indirect_block[third_index] = new_block_num;
            *was_allocated = true;
            block_ptr_write(fs, double_indirect_block[second_index],
                            (uint8_t *) single_indirect_block);
            block_num = new_block_num;
        }

        block_ptr_write(fs, triple_indirect_block[first_index],
                        (uint8_t *) double_indirect_block);
        block_ptr_write(fs, inode->block[14],
                        (uint8_t *) triple_indirect_block);

        kfree(single_indirect_block);
        kfree(double_indirect_block);
        kfree(triple_indirect_block);
        return block_num;
    }

    return (uint32_t) -1;
}

bool ext2_write_file(struct ext2_fs *fs, struct k_full_inode *inode,
                     uint32_t offset, const uint8_t *src, uint32_t size) {
    if (!fs || !inode || !src)
        return false;
    uint32_t bytes_written = 0;
    uint32_t new_block_counter = 0;
    while (bytes_written < size) {
        bool new_block = false;
        uint32_t file_offset = offset + bytes_written;
        uint32_t block_index = file_offset / fs->block_size;
        uint32_t block_offset = file_offset % fs->block_size;

        bool allocate = (size - bytes_written > 0);
        uint32_t block_num = get_or_set_block(fs, &inode->node, block_index, 0,
                                              allocate, &new_block);
        if (new_block) {
            new_block_counter += 1;
        }

        if (block_num == 0 && allocate) {
            return false;
        }

        if (block_num == 0) {
            bytes_written +=
                MIN(fs->block_size - block_offset, size - bytes_written);
            continue;
        }

        uint32_t to_write = fs->block_size - block_offset;
        if (to_write > size - bytes_written)
            to_write = size - bytes_written;

        uint8_t *block_buf = kmalloc(fs->block_size);
        if (!block_buf)
            return false;

        block_ptr_read(fs, block_num, block_buf);
        memcpy(block_buf + block_offset, src + bytes_written, to_write);
        block_ptr_write(fs, block_num, block_buf);

        kfree(block_buf);

        bytes_written += to_write;
    }

    inode->node.size = offset + size;
    inode->node.blocks += new_block_counter * (fs->block_size / 512);

    return ext2_write_inode(fs, inode->inode_num, &inode->node);
}
