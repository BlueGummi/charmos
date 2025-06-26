#include <console/printf.h>
#include <errno.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <string.h>
#include <time/time.h>

enum errno ext2_symlink_file(struct ext2_fs *fs,
                             struct ext2_full_inode *dir_inode,
                             const char *name, const char *target) {
    uint32_t inode_num = ext2_alloc_inode(fs);
    if (inode_num == 0)
        return ERR_FS_NO_INODE;

    struct ext2_inode new_inode = {0};
    new_inode.ctime = time_get_unix();
    new_inode.mtime = time_get_unix();
    new_inode.atime = time_get_unix();
    new_inode.mode = EXT2_S_IFLNK;
    new_inode.links_count = 1;
    new_inode.size = strlen(target);
    new_inode.blocks = 0;

    if (strlen(target) <= sizeof(new_inode.block)) {
        memcpy(new_inode.block, target, strlen(target));
        new_inode.block[strlen(target)] = '\0';
    } else {
        uint32_t block = ext2_alloc_block(fs);
        if (block == 0)
            return ERR_FS_NO_INODE;

        ext2_block_write(fs, block, target);
        new_inode.block[0] = block;
        new_inode.blocks = fs->block_size / fs->drive->sector_size;
    }

    if (!ext2_inode_write(fs, inode_num, &new_inode))
        return ERR_FS_INTERNAL;

    struct ext2_full_inode wrapped_inode = {
        .inode_num = inode_num,
        .node = new_inode,
    };

    enum errno ret = ext2_link_file(fs, dir_inode, &wrapped_inode,
                                    (char *) name, EXT2_FT_SYMLINK, false);
    return ret;
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
    if (!block)
        return ERR_NO_MEM;

    ext2_block_read(fs, first_block, block);

    if (!block)
        return ERR_IO;

    memcpy(buf, block, link_size > block_size ? block_size : link_size);
    return 0;
}
