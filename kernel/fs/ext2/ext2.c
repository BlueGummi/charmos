#include <console/printf.h>
#include <errno.h>
#include <fs/ext2.h>
#include <fs/ext2_print.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <string.h>
#include <time/time.h>

uint64_t PTRS_PER_BLOCK;

bool ext2_read_superblock(struct generic_partition *p,
                          struct ext2_sblock *sblock) {
    struct generic_disk *d = p->disk;
    uint8_t *buffer = kmalloc(d->sector_size);
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

enum errno ext2_mount(struct generic_partition *d, struct ext2_fs *fs,
                      struct ext2_sblock *sblock) {
    if (!fs || !sblock)
        return ERR_INVAL;

    sblock->mtime = time_get_unix();
    sblock->wtime = time_get_unix();
    fs->drive = d->disk;
    fs->partition = d;
    fs->sblock = sblock;
    fs->inodes_count = sblock->inodes_count;
    fs->inodes_per_group = sblock->inodes_per_group;
    fs->inode_size = sblock->inode_size;
    fs->block_size = 1024U << sblock->log_block_size;
    fs->sectors_per_block = fs->block_size / d->disk->sector_size;

    fs->num_groups =
        (fs->inodes_count + fs->inodes_per_group - 1) / fs->inodes_per_group;

    uint32_t gdt_block = (fs->block_size == 1024) ? 2 : 1;

    uint32_t gdt_bytes = fs->num_groups * sizeof(struct ext2_group_desc);
    uint32_t gdt_blocks = (gdt_bytes + fs->block_size - 1) / fs->block_size;

    fs->group_desc = kmalloc(gdt_blocks * fs->block_size);
    if (!fs->group_desc)
        return ERR_NO_MEM;

    if (!ext2_block_read(fs->partition, gdt_block * fs->sectors_per_block,
                         (uint8_t *) fs->group_desc,
                         gdt_blocks * fs->sectors_per_block)) {
        kfree(fs->group_desc);
        return ERR_FS_INTERNAL;
    }

    return ERR_OK;
}

enum errno ext2_g_mount(struct generic_partition *p) {
    if (!p)
        return ERR_INVAL;
    p->fs_data = kmalloc(sizeof(struct ext2_fs));
    struct ext2_fs *fs = p->fs_data;
    fs->sblock = kmalloc(sizeof(struct ext2_sblock));

    if (!ext2_read_superblock(p, fs->sblock))
        return ERR_FS_INTERNAL;

    return ext2_mount(p, fs, fs->sblock);
}

void ext2_g_print(struct generic_partition *p) {
    if (!p)
        return;

    struct ext2_fs *fs = p->fs_data;
    ext2_print_superblock(fs->sblock);
}

struct ext2_full_inode *ext2_path_lookup(struct ext2_fs *fs,
                                         struct ext2_full_inode *node,
                                         const char *path) {
    if (!path || !fs || !node)
        return NULL;

    while (*path == '/')
        path++;

    if (*path == '\0')
        return node;

    const char *start = path;
    while (*path && *path != '/')
        path++;

    uint64_t len = (uint64_t) path - (uint64_t) start;
    char next_dir[len + 1];
    memcpy(next_dir, start, len);
    next_dir[len] = '\0';

    struct ext2_full_inode *next = ext2_find_file_in_dir(fs, node, next_dir, NULL);

    if (!next) {
        k_printf("Did not find %s\n", next_dir);
        return NULL;
    }

    return ext2_path_lookup(fs, next, path);
}

void ext2_test(struct generic_partition *p, struct ext2_sblock *sblock) {
    struct ext2_fs fs;
    if (ERR_IS_FATAL(ext2_mount(p, &fs, sblock))) {
        return;
    }

    struct ext2_inode r;
    if (!ext2_read_inode(&fs, EXT2_ROOT_INODE, &r)) {
        return;
    }
}
