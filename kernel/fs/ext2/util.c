#include <fs/ext2.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <string.h>

static uint32_t ext2_get_block(struct ext2_fs *fs, uint32_t block_num,
                               uint32_t depth, uint32_t block_index,
                               uint32_t new_block_num, bool allocate,
                               bool *was_allocated) {
    if (depth == 0) {
        // actual data block
        return block_num;
    }

    uint32_t pointers_per_block = fs->block_size / sizeof(uint32_t);
    uint32_t *block = kmalloc(fs->block_size);
    if (!block)
        return 0;

    bool allocated_this_level = false;
    if (block_num == 0) {
        if (!allocate) {
            kfree(block);
            return 0;
        }

        block_num = ext2_alloc_block(fs);
        if (block_num == 0) {
            kfree(block);
            return 0;
        }

        allocated_this_level = true;
        memset(block, 0, fs->block_size);
        ext2_block_ptr_write(fs, block_num, (uint8_t *) block);
    } else {
        ext2_block_ptr_read(fs, block_num, (uint8_t *) block);
    }

    uint32_t index = block_index;
    uint32_t divisor = 1;
    for (uint32_t i = 1; i < depth; ++i)
        divisor *= pointers_per_block;

    uint32_t entry_index = index / divisor;
    uint32_t entry_offset = index % divisor;

    uint32_t result =
        ext2_get_block(fs, block[entry_index], depth - 1, entry_offset,
                       new_block_num, allocate, was_allocated);

    if (result && block[entry_index] == 0 && allocate) {
        block[entry_index] = result;
        ext2_block_ptr_write(fs, block_num, (uint8_t *) block);
    } else if (allocated_this_level && result == 0) {
        ext2_free_block(fs, block_num);
    }

    kfree(block);
    return result;
}

uint32_t ext2_get_or_set_block(struct ext2_fs *fs, struct ext2_inode *inode,
                               uint32_t block_index, uint32_t new_block_num,
                               bool allocate, bool *was_allocated) {
    if (!fs || !inode)
        return (uint32_t) -1;

    if (was_allocated)
        *was_allocated = false;

    uint32_t pointers_per_block = fs->block_size / sizeof(uint32_t);

    if (block_index < 12) {
        if (inode->block[block_index] == 0 && allocate) {
            if (new_block_num == 0)
                new_block_num = ext2_alloc_block(fs);

            if (new_block_num == 0)
                return 0;

            inode->block[block_index] = new_block_num;
            if (was_allocated)
                *was_allocated = true;
        }
        return inode->block[block_index];
    }

    block_index -= 12;

    if (block_index < pointers_per_block) {
        return ext2_get_block(fs, inode->block[12], 1, block_index,
                              new_block_num, allocate, was_allocated);
    }

    block_index -= pointers_per_block;

    if (block_index < pointers_per_block * pointers_per_block) {
        return ext2_get_block(fs, inode->block[13], 2, block_index,
                              new_block_num, allocate, was_allocated);
    }

    block_index -= pointers_per_block * pointers_per_block;

    uint64_t max_triple =
        (uint64_t) pointers_per_block * pointers_per_block * pointers_per_block;
    if ((uint64_t) block_index < max_triple) {
        return ext2_get_block(fs, inode->block[14], 3, block_index,
                              new_block_num, allocate, was_allocated);
    }

    return (uint32_t) -1;
}
