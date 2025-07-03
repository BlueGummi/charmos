#include <errno.h>
#include <fs/ext2.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs/generic.h"
#include "fs/bcache.h"

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
            ent = ext2_block_read(fs, inode->block[EXT2_IND_BLOCK]);
            if (!ent)
                return;

            bcache_ent_lock(ent);
            uint32_t *ind = (uint32_t *) ent->buffer;
            ind[block_index - EXT2_NDIR_BLOCKS] = 0;
            bcache_ent_unlock(ent);

            ext2_block_write(fs, ent);
        }
    } else if (block_index < EXT2_NDIR_BLOCKS + bpi + bpi * bpi) {
        uint32_t dbl_index = block_index - EXT2_NDIR_BLOCKS - bpi;
        uint32_t ind1 = dbl_index / bpi;
        uint32_t ind2 = dbl_index % bpi;

        if (inode->block[EXT2_DIND_BLOCK]) {
            i2 = ext2_block_read(fs, inode->block[EXT2_DIND_BLOCK]);
            if (!i2)
                return;

            bcache_ent_lock(i2);
            uint32_t *dind = (uint32_t *) i2->buffer;

            if (dind[ind1]) {
                ent = ext2_block_read(fs, dind[ind1]);
                if (!ent)
                    return;

                bcache_ent_lock(ent);
                uint32_t *ind = (uint32_t *) ent->buffer;
                ind[ind2] = 0;
                bcache_ent_unlock(ent);

                ext2_block_write(fs, ent);
            }
            bcache_ent_unlock(i2);
        }

    } else {
        uint32_t tpl_index = block_index - EXT2_NDIR_BLOCKS - bpi - bpi * bpi;
        uint32_t ind1 = tpl_index / (bpi * bpi);
        uint32_t rem = tpl_index % (bpi * bpi);
        uint32_t ind2 = rem / bpi;
        uint32_t ind3 = rem % bpi;

        if (inode->block[EXT2_TIND_BLOCK]) {

            i3 = ext2_block_read(fs, inode->block[EXT2_TIND_BLOCK]);
            if (!i3)
                return;

            bcache_ent_lock(i3);
            uint32_t *tind = (uint32_t *) i3->buffer;

            if (tind[ind1]) {
                i2 = ext2_block_read(fs, tind[ind1]);
                if (!i2)
                    return;

                bcache_ent_lock(i2);
                uint32_t *dind = (uint32_t *) i2->buffer;

                if (dind[ind2]) {
                    ent = ext2_block_read(fs, dind[ind2]);

                    bcache_ent_lock(ent);
                    uint32_t *ind = (uint32_t *) ent->buffer;
                    ind[ind3] = 0;
                    bcache_ent_unlock(ent);

                    ext2_block_write(fs, ent);
                }
                bcache_ent_unlock(i2);
            }
            bcache_ent_unlock(i3);
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
