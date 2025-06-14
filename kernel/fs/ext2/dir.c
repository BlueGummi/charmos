#include <console/printf.h>
#include <errno.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <string.h>
#include <time/time.h>

enum errno ext2_mkdir(struct ext2_fs *fs, struct ext2_full_inode *parent_dir,
                      const char *name, uint16_t mode) {
    if (!(mode & EXT2_S_IFDIR)) {
        mode |= EXT2_S_IFDIR;
    }

    enum errno err = ext2_create_file(fs, parent_dir, name, mode);
    if (err != ERR_OK)
        return err;

    uint8_t type;
    struct ext2_full_inode *dir =
        ext2_find_file_in_dir(fs, parent_dir, name, &type);
    if (!dir)
        return ERR_IO;

    uint8_t *block = kmalloc(fs->block_size);
    memset(block, 0, fs->block_size);

    // "." entry
    struct ext2_dir_entry *dot = (struct ext2_dir_entry *) block;
    dot->inode = dir->inode_num;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    strcpy(dot->name, ".");

    // ".." entry
    struct ext2_dir_entry *dotdot = (struct ext2_dir_entry *) (block + 12);
    dotdot->inode = parent_dir->inode_num;
    dotdot->rec_len = fs->block_size - 12;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    strcpy(dotdot->name, "..");

    bool was_allocated;
    uint32_t new_block =
        ext2_get_or_set_block(fs, &dir->node, 0, 0, true, &was_allocated);
    if (new_block == 0) {
        kfree(block);
        return ERR_NOSPC;
    }
    ext2_block_ptr_write(fs, new_block, block);
    kfree(block);

    dir->node.size = fs->block_size;
    dir->node.links_count = 2; // . and ..
    ext2_write_inode(fs, dir->inode_num, &dir->node);

    parent_dir->node.links_count++;
    ext2_write_inode(fs, parent_dir->inode_num, &parent_dir->node);

    return ERR_OK;
}

enum errno ext2_rmdir(struct ext2_fs *fs, struct ext2_full_inode *parent_dir,
                      const char *name) {
    uint8_t type;
    struct ext2_full_inode *dir =
        ext2_find_file_in_dir(fs, parent_dir, name, &type);
    if (!dir || !(dir->node.mode & EXT2_S_IFDIR))
        return ERR_NO_ENT;

    uint8_t *block = kmalloc(fs->block_size);
    if (!ext2_block_ptr_read(
            fs, ext2_get_or_set_block(fs, &dir->node, 0, 0, false, NULL),
            block)) {
        kfree(block);
        return ERR_IO;
    }

    bool empty = true;
    uint32_t offset = 0;
    while (offset < dir->node.size) {
        struct ext2_dir_entry *entry =
            (struct ext2_dir_entry *) (block + offset);
        if (entry->name_len == 1 && entry->name[0] == '.') {
            // skip
        } else if (entry->name_len == 2 && entry->name[0] == '.' &&
                   entry->name[1] == '.') {
            // skip
        } else {
            empty = false;
            break;
        }
        offset += entry->rec_len;
    }

    kfree(block);
    if (!empty)
        return ERR_NOT_EMPTY;

    enum errno err = ext2_unlink_file(fs, parent_dir, name, true);
    if (err != ERR_OK)
        return err;

    parent_dir->node.links_count--;
    ext2_write_inode(fs, parent_dir->inode_num, &parent_dir->node);

    return ERR_OK;
}
