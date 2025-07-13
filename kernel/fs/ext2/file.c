#include <errno.h>
#include <fs/ext2.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum errno ext2_write_file(struct ext2_fs *fs, struct ext2_full_inode *inode,
                           uint32_t offset, const uint8_t *src, uint32_t size) {
    if (!fs || !inode || !src)
        return ERR_INVAL;

    if (inode->node.mode & EXT2_S_IFDIR)
        return ERR_IS_DIR;

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

        struct bcache_entry *ent;
        uint8_t *block_buf = ext2_block_read(fs, block_num, &ent);
        if (!block_buf)
            return ERR_IO;

        bcache_ent_acquire(ent);
        memcpy(block_buf + block_offset, src + bytes_written, to_write);
        bcache_ent_release(ent);

        ext2_block_write(fs, ent, EXT2_PRIO_DATA);

        bytes_written += to_write;
    }

    if (offset + size > inode->node.size) {
        inode->node.size = offset + size;
    }

    inode->node.blocks +=
        new_block_counter * (fs->block_size / fs->drive->sector_size);

    bool status = ext2_inode_write(fs, inode->inode_num, &inode->node);
    return status ? ERR_OK : ERR_IO;
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
    struct bcache_entry *ent;
    uint8_t *block_buf = ext2_block_read(fs, *block_ptr, &ent);
    if (!block_buf)
        return;

    bcache_ent_acquire(ent);

    uint32_t file_offset = ctx->bytes_read + ctx->offset;
    uint32_t block_offset = file_offset % block_size;

    if ((ctx->bytes_read + ctx->offset) >= inode->size) {
        return;
    }

    uint32_t remaining = ctx->length - ctx->bytes_read;
    uint32_t in_block = block_size - block_offset;
    uint32_t to_copy = (remaining < in_block) ? remaining : in_block;

    if ((ctx->bytes_read + ctx->offset + to_copy) > inode->size)
        to_copy = inode->size - (ctx->bytes_read + ctx->offset);

    memcpy(ctx->buffer + ctx->bytes_read, block_buf + block_offset, to_copy);
    ctx->bytes_read += to_copy;
    bcache_ent_release(ent);
}

enum errno ext2_read_file(struct ext2_fs *fs, struct ext2_full_inode *inode,
                          uint32_t offset, uint8_t *buffer, uint64_t length) {
    if (!fs || !inode || !buffer || offset >= inode->node.size)
        return ERR_INVAL;

    if (inode->node.mode & EXT2_S_IFDIR)
        return ERR_IS_DIR;

    if (offset + length > inode->node.size)
        length = inode->node.size - offset;

    struct file_read_ctx ctx = {.fs = fs,
                                .inode = &inode->node,
                                .offset = offset,
                                .length = length,
                                .buffer = buffer,
                                .bytes_read = 0};

    ext2_traverse_inode_blocks(fs, &inode->node, file_read_visitor, &ctx, true);
    inode->node.atime = time_get_unix();
    ext2_inode_write(fs, inode->inode_num, &inode->node);
    return ERR_OK;
}

enum errno ext2_chmod(struct ext2_fs *fs, struct ext2_full_inode *node,
                      uint16_t new_mode) {
    if (!fs || !node)
        return ERR_INVAL;

    uint16_t ftype = node->node.mode & EXT2_S_IFMT;
    node->node.mode = ftype | (new_mode & EXT2_S_PERMS);

    if (!ext2_inode_write(fs, node->inode_num, &node->node))
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

    if (!ext2_inode_write(fs, node->inode_num, &node->node))
        return ERR_FS_INTERNAL;

    return ERR_OK;
}
