#include <errno.h>
#include <fs/ext2.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK EXT2_NDIR_BLOCKS
#define EXT2_DIND_BLOCK (EXT2_IND_BLOCK + 1)
#define EXT2_TIND_BLOCK (EXT2_DIND_BLOCK + 1)
#define EXT2_N_BLOCKS (EXT2_TIND_BLOCK + 1)

static uint32_t blocks_per_indirection(struct ext2_fs *fs) {
    return fs->block_size / sizeof(uint32_t);
}

static void clear_block_pointer(struct ext2_fs *fs, struct ext2_inode *inode,
                                uint32_t block_index) {
    uint32_t bpi = blocks_per_indirection(fs);

    /* current entry and indirection levels */
    struct bcache_entry *ent, *i2, *i3;

    if (block_index < EXT2_NDIR_BLOCKS) {
        inode->block[block_index] = 0;
    } else if (block_index < EXT2_NDIR_BLOCKS + bpi) {
        if (inode->block[EXT2_IND_BLOCK]) {
            uint32_t *ind = (uint32_t *) ext2_block_read(
                fs, inode->block[EXT2_IND_BLOCK], &ent);

            if (!ind)
                return;

            ind[block_index - EXT2_NDIR_BLOCKS] = 0;
            bcache_ent_release(ent);

            ext2_block_write(fs, ent, EXT2_PRIO_INODE);
        }
    } else if (block_index < EXT2_NDIR_BLOCKS + bpi + bpi * bpi) {
        uint32_t dbl_index = block_index - EXT2_NDIR_BLOCKS - bpi;
        uint32_t ind1 = dbl_index / bpi;
        uint32_t ind2 = dbl_index % bpi;

        if (inode->block[EXT2_DIND_BLOCK]) {
            uint32_t *dind = (uint32_t *) ext2_block_read(
                fs, inode->block[EXT2_DIND_BLOCK], &i2);
            if (!dind)
                return;

            if (dind[ind1]) {
                uint32_t *ind =
                    (uint32_t *) ext2_block_read(fs, dind[ind1], &ent);
                if (!ind)
                    return;

                ind[ind2] = 0;
                bcache_ent_release(ent);

                ext2_block_write(fs, ent, EXT2_PRIO_INODE);
            }
            bcache_ent_release(i2);
        }

    } else {
        uint32_t tpl_index = block_index - EXT2_NDIR_BLOCKS - bpi - bpi * bpi;
        uint32_t ind1 = tpl_index / (bpi * bpi);
        uint32_t rem = tpl_index % (bpi * bpi);
        uint32_t ind2 = rem / bpi;
        uint32_t ind3 = rem % bpi;

        if (inode->block[EXT2_TIND_BLOCK]) {

            uint32_t *tind = (uint32_t *) ext2_block_read(
                fs, inode->block[EXT2_TIND_BLOCK], &i3);
            if (!tind)
                return;

            if (tind[ind1]) {
                uint32_t *dind =
                    (uint32_t *) ext2_block_read(fs, tind[ind1], &i2);
                if (!dind) {
                    bcache_ent_release(i3);
                    return;
                }

                if (dind[ind2]) {
                    uint32_t *ind =
                        (uint32_t *) ext2_block_read(fs, dind[ind2], &ent);

                    ind[ind3] = 0;
                    bcache_ent_release(ent);

                    ext2_block_write(fs, ent, EXT2_PRIO_INODE);
                }
                bcache_ent_release(i2);
            }
            bcache_ent_release(i3);
        }
    }
}

enum errno ext2_truncate_file(struct ext2_fs *fs, struct ext2_full_inode *inode,
                              uint32_t new_size) {
    uint32_t old_block_count =
        (inode->node.size + fs->block_size - 1) / fs->block_size;
    uint32_t new_block_count = (new_size + fs->block_size - 1) / fs->block_size;
    uint32_t bpi = blocks_per_indirection(fs);

    for (uint32_t i = new_block_count; i < old_block_count; i++) {
        uint32_t block_num =
            ext2_get_or_set_block(fs, &inode->node, i, 1, false, NULL);
        if (block_num) {
            ext2_free_block(fs, block_num);
            clear_block_pointer(fs, &inode->node, i);
        }
    }

    if (new_block_count <= EXT2_NDIR_BLOCKS &&
        inode->node.block[EXT2_IND_BLOCK]) {
        ext2_free_block(fs, inode->node.block[EXT2_IND_BLOCK]);
        inode->node.block[EXT2_IND_BLOCK] = 0;
    }

    if (new_block_count <= EXT2_NDIR_BLOCKS + bpi &&
        inode->node.block[EXT2_DIND_BLOCK]) {
        ext2_free_block(fs, inode->node.block[EXT2_DIND_BLOCK]);
        inode->node.block[EXT2_DIND_BLOCK] = 0;
    }

    if (new_block_count <= EXT2_NDIR_BLOCKS + bpi + bpi * bpi &&
        inode->node.block[EXT2_TIND_BLOCK]) {
        ext2_free_block(fs, inode->node.block[EXT2_TIND_BLOCK]);
        inode->node.block[EXT2_TIND_BLOCK] = 0;
    }

    inode->node.size = new_size;
    inode->node.blocks =
        new_block_count * (fs->block_size / fs->drive->sector_size);

    return ext2_inode_write(fs, inode->inode_num, &inode->node)
               ? ERR_OK
               : ERR_FS_INTERNAL;
}
