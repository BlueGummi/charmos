#include <errno.h>
#include <fs/ext2.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool ext2_dirent_valid(struct ext2_dir_entry *entry) {
    if (entry->inode == 0 || entry->rec_len < 8 || entry->name_len == 0)
        return false;

    return true;
}

static void init_dir(struct ext2_fs *fs, struct ext2_full_inode *dir,
                     uint32_t new_block) {
    dir->node.block[0] = new_block;
    dir->node.size = fs->block_size;
    dir->node.blocks = 2;
    dir->node.links_count = 2;
}

static void init_dot_ents(struct ext2_fs *fs, uint8_t *block,
                          struct ext2_full_inode *parent_dir,
                          struct ext2_full_inode *dir) {
    struct ext2_dir_entry *dot = (struct ext2_dir_entry *) block;
    dot->inode = dir->inode_num;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    strcpy(dot->name, ".");

    struct ext2_dir_entry *dotdot = (struct ext2_dir_entry *) (block + 12);
    dotdot->inode = parent_dir->inode_num;
    dotdot->rec_len = fs->block_size - 12;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    strcpy(dotdot->name, "..");
}

enum errno ext2_mkdir(struct ext2_fs *fs, struct ext2_full_inode *parent_dir,
                      const char *name, uint16_t mode) {
    if (!(mode & EXT2_S_IFDIR))
        mode |= EXT2_S_IFDIR;

    enum errno err = ext2_create_file(fs, parent_dir, name, mode, true);
    if (err != ERR_OK)
        return err;

    struct ext2_full_inode *dir;
    dir = ext2_find_file_in_dir(fs, parent_dir, name, NULL);
    if (!dir)
        return ERR_IO;

    uint32_t new_block = ext2_alloc_block(fs);
    if (new_block == 0)
        return ERR_NOSPC;

    struct bcache_entry *ent;
    uint8_t *block = ext2_create_bcache_ent(fs, new_block, &ent);

    if (!block)
        return ERR_IO;

    bcache_ent_lock(ent);
    init_dot_ents(fs, block, parent_dir, dir);
    bcache_ent_unlock(ent);

    init_dir(fs, dir, new_block);

    ext2_inode_write(fs, dir->inode_num, &dir->node);
    ext2_inode_write(fs, parent_dir->inode_num, &parent_dir->node);
    ext2_block_write(fs, ent, EXT2_PRIO_DIRENT);

    return ERR_OK;
}

enum errno ext2_rmdir(struct ext2_fs *fs, struct ext2_full_inode *parent_dir,
                      const char *name) {
    uint8_t type;
    struct ext2_full_inode *dir;
    dir = ext2_find_file_in_dir(fs, parent_dir, name, &type);
    if (!dir)
        return ERR_NO_ENT;

    if (!(dir->node.mode & EXT2_S_IFDIR))
        return ERR_NOT_DIR;

    uint32_t b_idx = 0;
    uint32_t b_num = 0;

    uint32_t tmp =
        ext2_get_or_set_block(fs, &dir->node, b_idx, b_num, false, NULL);

    struct bcache_entry *ent;
    uint8_t *block = ext2_block_read(fs, tmp, &ent);

    if (!ent)
        return ERR_IO;

    bcache_ent_lock(ent);

    bool empty = true;
    uint32_t offset = 0;

    while (offset < dir->node.size) {
        struct ext2_dir_entry *entry;
        entry = (struct ext2_dir_entry *) (block + offset);
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

    bcache_ent_unlock(ent);
    if (!empty)
        return ERR_NOT_EMPTY;

    bool free_blocks = true;
    bool decrement_links = true;

    enum errno err =
        ext2_unlink_file(fs, parent_dir, name, free_blocks, decrement_links);

    if (err != ERR_OK)
        return err;

    if (dir->node.blocks) {
        uint32_t block = dir->node.block[0];
        if (block)
            ext2_free_block(fs, block);
        dir->node.block[0] = 0;
        dir->node.blocks = 0;
        dir->node.size = 0;
    }

    uint32_t group = ext2_get_inode_group(fs, dir->inode_num);
    struct ext2_group_desc *desc = &fs->group_desc[group];

    bool i = ext2_fs_lock(fs);
    desc->used_dirs_count--;
    parent_dir->node.links_count--;
    ext2_fs_unlock(fs, i);

    ext2_free_inode(fs, dir->inode_num);

    ext2_inode_write(fs, dir->inode_num, &dir->node);
    ext2_inode_write(fs, parent_dir->inode_num, &parent_dir->node);

    return ERR_OK;
}
