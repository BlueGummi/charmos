#include <mem/alloc.h>
#include <errno.h>
#include <fs/ext2.h>
#include <console/printf.h>
#include <string.h>

enum errno ext2_write_file(struct ext2_fs *fs, struct k_full_inode *inode,
                           uint32_t offset, const uint8_t *src, uint32_t size) {
    if (!fs || !inode || !src)
        return ERR_INVAL;

    uint32_t bytes_written = 0;
    uint32_t new_block_counter = 0;
    while (bytes_written < size) {
        bool new_block = false;
        uint32_t file_offset = offset + bytes_written;
        uint32_t block_index = file_offset / fs->block_size;
        uint32_t block_offset = file_offset % fs->block_size;

        bool allocate = (size - bytes_written > 0);
        uint32_t block_num = ext2_get_or_set_block(
            fs, &inode->node, block_index, 0, allocate, &new_block);
        if (new_block) {
            new_block_counter += 1;
        }

        if (block_num == 0 && allocate) {
            return ERR_FS_NO_INODE;
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
            return ERR_NO_MEM;

        ext2_block_ptr_read(fs, block_num, block_buf);
        memcpy(block_buf + block_offset, src + bytes_written, to_write);
        ext2_block_ptr_write(fs, block_num, block_buf);

        kfree(block_buf);

        bytes_written += to_write;
    }

    if (offset + size < inode->node.size) {
        inode->node.size = offset + size;
    }

    inode->node.blocks +=
        new_block_counter * (fs->block_size / fs->drive->sector_size);

    return ext2_write_inode(fs, inode->inode_num, &inode->node)
               ? ERR_OK
               : ERR_FS_INTERNAL;
}
