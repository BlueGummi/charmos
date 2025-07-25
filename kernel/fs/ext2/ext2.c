#include <fs/ext2.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// TODO: flags - maybe?

uint64_t PTRS_PER_BLOCK;

bool ext2_read_superblock(struct generic_partition *p,
                          struct ext2_sblock *sblock) {
    struct generic_disk *d = p->disk;
    uint8_t *buffer = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (!buffer)
        return false; // TODO: separate ERR_NO_MEM case

    memset(buffer, 0, PAGE_SIZE);
    uint32_t superblock_lba = (EXT2_SUPERBLOCK_OFFSET / d->sector_size);
    uint32_t superblock_offset = EXT2_SUPERBLOCK_OFFSET % d->sector_size;

    if (!d->read_sector(d, superblock_lba + p->start_lba, buffer, 1)) {
        kfree_aligned(buffer);
        return false;
    }

    memcpy(sblock, buffer + superblock_offset, sizeof(struct ext2_sblock));

    kfree_aligned(buffer);
    return (sblock->magic == 0xEF53);
}

bool ext2_write_superblock(struct ext2_fs *fs) {
    return ext2_block_write(fs, fs->sbcache_ent, EXT2_PRIO_SBLOCK);
}

bool ext2_write_group_desc(struct ext2_fs *fs) {
    return ext2_block_write(fs, fs->gdesc_cache_ent, EXT2_PRIO_SBLOCK);
}

struct vfs_node *ext2_g_mount(struct generic_partition *p) {
    if (!p)
        return NULL;

    p->fs_data = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    struct ext2_fs *fs = p->fs_data;
    fs->sblock = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);

    memset(fs->sblock, 0, PAGE_SIZE);

    struct vfs_node *n = kzalloc(sizeof(struct vfs_node));

    if (!p->fs_data || !fs->sblock | !n)
        return NULL;

    if (!ext2_read_superblock(p, fs->sblock))
        return NULL;

    ext2_mount(p, fs, fs->sblock, n);
    return n;
}

void ext2_g_print(struct generic_partition *p) {
    if (!p)
        return;
}
