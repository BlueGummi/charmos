#include <console/printf.h>
#include <errno.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <string.h>

enum errno ext2_write_file(struct ext2_fs *fs, struct ext2_full_inode *inode,
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

struct file_read_ctx {
    struct ext2_fs *fs;
    struct ext2_inode *inode;
    uint32_t offset;
    uint32_t length;
    uint8_t *buffer;
    uint32_t bytes_read;
};

static void file_read_visitor(struct ext2_fs *fs, struct ext2_inode *inode,
                              uint32_t depth, uint32_t *block_ptr,
                              void *user_data) {
    (void) depth;
    struct file_read_ctx *ctx = (struct file_read_ctx *) user_data;

    if (ctx->bytes_read >= ctx->length)
        return;

    if (*block_ptr == 0)
        return;

    uint32_t block_size = fs->block_size;
    uint8_t *block_buf = kmalloc(block_size);
    uint32_t lba = (*block_ptr) * fs->sectors_per_block;

    if (!ext2_block_read(fs->partition, lba, block_buf,
                         fs->sectors_per_block)) {
        kfree(block_buf);
        return;
    }

    uint32_t file_offset = ctx->bytes_read + ctx->offset;
    uint32_t block_offset = file_offset % block_size;

    if ((ctx->bytes_read + ctx->offset) >= inode->size) {
        kfree(block_buf);
        return;
    }

    uint32_t remaining = ctx->length - ctx->bytes_read;
    uint32_t in_block = block_size - block_offset;
    uint32_t to_copy = (remaining < in_block) ? remaining : in_block;

    if ((ctx->bytes_read + ctx->offset + to_copy) > inode->size)
        to_copy = inode->size - (ctx->bytes_read + ctx->offset);

    memcpy(ctx->buffer + ctx->bytes_read, block_buf + block_offset, to_copy);
    ctx->bytes_read += to_copy;
    ctx->offset += to_copy;
    kfree(block_buf);
}

enum errno ext2_read_file(struct ext2_fs *fs, struct ext2_full_inode *inode,
                          uint32_t offset, uint8_t *buffer, uint64_t length) {
    if (!fs || !inode || !buffer || offset >= inode->node.size)
        return ERR_INVAL;

    if (offset + length > inode->node.size)
        length = inode->node.size - offset;

    struct file_read_ctx ctx = {.fs = fs,
                                .inode = &inode->node,
                                .offset = offset,
                                .length = length,
                                .buffer = buffer,
                                .bytes_read = 0};

    ext2_traverse_inode_blocks(fs, &inode->node, file_read_visitor, &ctx);
    return ERR_OK;
}

enum errno ext2_chmod(struct ext2_fs *fs, struct ext2_full_inode *node,
                      uint16_t new_mode) {
    if (!fs || !node)
        return ERR_INVAL;

    uint16_t ftype = node->node.mode & EXT2_S_IFMT;
    node->node.mode = ftype | (new_mode & EXT2_S_PERMS);

    if (!ext2_write_inode(fs, node->inode_num, &node->node))
        return ERR_FS_INTERNAL;

    return ERR_OK;
}

enum errno ext2_chown(struct ext2_fs *fs, struct ext2_full_inode *node,
                      uint32_t new_uid, uint32_t new_gid) {
    if (!fs || !node)
        return ERR_INVAL;

    if (new_uid != (uint32_t) -1)
        node->node.uid = new_uid;

    if (new_gid != (uint32_t) -1)
        node->node.gid = new_gid;

    if (!ext2_write_inode(fs, node->inode_num, &node->node))
        return ERR_FS_INTERNAL;

    return ERR_OK;
}

enum errno ext2_readlink(struct ext2_fs *fs, struct ext2_full_inode *node,
                         char *buf, uint64_t size) {
    if (!fs || !node || !buf)
        return ERR_INVAL;

    uint64_t link_size = node->node.size;

    if (link_size > size)
        link_size = size;

    // inline data stored in i_block[]
    if (link_size <= 60) {
        memcpy(buf, node->node.block, link_size);
        return 0;
    }

    // target is stored in data blocks
    uint32_t block_size = 1024 << fs->sblock->log_block_size;
    uint32_t first_block = node->node.block[0];

    if (first_block == 0)
        return ERR_IO;

    void *block = kmalloc(fs->block_size);

    ext2_block_ptr_read(fs, first_block, block);

    if (!block)
        return ERR_IO;

    memcpy(buf, block, link_size > block_size ? block_size : link_size);
    return 0;
}
