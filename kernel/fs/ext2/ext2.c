#include <console/printf.h>
#include <fs/ext2.h>
#include <fs/ext2_print.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <string.h>
#include <time/time.h>

// TODO: flags - maybe?

uint64_t PTRS_PER_BLOCK;

bool ext2_read_superblock(struct generic_partition *p,
                          struct ext2_sblock *sblock) {
    struct generic_disk *d = p->disk;
    uint8_t *buffer = kmalloc(d->sector_size);
    if (!buffer)
        return false; // TODO: separate ERR_NO_MEM case

    uint32_t superblock_lba = (EXT2_SUPERBLOCK_OFFSET / d->sector_size);
    uint32_t superblock_offset = EXT2_SUPERBLOCK_OFFSET % d->sector_size;

    if (!d->read_sector(d, superblock_lba + p->start_lba, buffer, 1)) {
        kfree(buffer);
        return false;
    }

    memcpy(sblock, buffer + superblock_offset, sizeof(struct ext2_sblock));

    kfree(buffer);
    return (sblock->magic == 0xEF53);
}

bool ext2_write_superblock(struct ext2_fs *fs) {
    uint32_t superblock_block = 1;
    uint32_t lba = superblock_block * fs->sectors_per_block;

    return ext2_block_write(fs->partition, lba, (uint8_t *) fs->sblock,
                            fs->sectors_per_block);
}

bool ext2_write_group_desc(struct ext2_fs *fs) {
    uint32_t group_desc_block = (fs->block_size == 1024) ? 2 : 1;
    uint32_t lba = group_desc_block * fs->sectors_per_block;

    uint32_t size = fs->num_groups * sizeof(struct ext2_group_desc);
    uint32_t sector_size = fs->drive->sector_size;
    uint32_t sector_count = (size + (sector_size - 1)) / sector_size;

    return ext2_block_write(fs->partition, lba, (uint8_t *) fs->group_desc,
                            sector_count);
}

struct vfs_node *ext2_g_mount(struct generic_partition *p) {
    if (!p)
        return NULL;

    struct ext2_fs *fs = p->fs_data;
    p->fs_data = kmalloc(sizeof(struct ext2_fs));
    fs->sblock = kmalloc(sizeof(struct ext2_sblock));
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

    struct ext2_fs *fs = p->fs_data;
    ext2_print_superblock(fs->sblock);
}

void ext2_test(struct generic_partition *p, struct ext2_sblock *sblock) {
    struct ext2_fs fs;
    if (ERR_IS_FATAL(ext2_mount(p, &fs, sblock, NULL))) {
        return;
    }

    struct ext2_inode r;
    if (!ext2_read_inode(&fs, EXT2_ROOT_INODE, &r)) {
        return;
    }

    //    ext2_link_file(&fs, &root_inode, &i, "file");
    ext2_print_superblock(sblock);
    struct ext2_full_inode f = {.node = r, .inode_num = 2};
    ext2_print_inode(&f);
    //    ext2_write_file(&fs, &i, 0, (uint8_t *) data, strlen(data));
    //    ext2_dump_file_data(&fs, &i.node, 0, strlen(data));
}
