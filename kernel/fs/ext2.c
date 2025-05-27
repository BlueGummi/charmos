#include <fs/ext2.h>
#include <fs/ext2_print.h>
#include <printf.h>
#include <stdint.h>
#include <string.h>
#include <vmalloc.h>

uint64_t PTRS_PER_BLOCK;

bool ext2_write_superblock(struct ext2_fs *fs) {
    uint32_t superblock_block = 1;
    uint32_t lba = superblock_block * fs->sectors_per_block;

    return block_write(fs->drive, lba, (uint8_t *) fs->sblock,
                       fs->sectors_per_block);
}

bool ext2_write_group_desc(struct ext2_fs *fs) {
    uint32_t group_desc_block = (fs->block_size == 1024) ? 2 : 1;
    uint32_t lba = group_desc_block * fs->sectors_per_block;

    uint32_t size = fs->num_groups * sizeof(struct ext2_group_desc);

    uint32_t sector_count = (size + 511) / fs->drive->sector_size;

    return block_write(fs->drive, lba, (uint8_t *) fs->group_desc,
                       sector_count);
}

bool ext2_mount(struct ide_drive *d, struct ext2_fs *fs,
                struct ext2_sblock *sblock) {
    if (!fs || !sblock)
        return false;

    fs->drive = d;
    fs->sblock = sblock;
    fs->inodes_count = sblock->inodes_count;
    fs->inodes_per_group = sblock->inodes_per_group;
    fs->inode_size = sblock->inode_size;
    fs->block_size = 1024 << sblock->log_block_size;
    fs->sectors_per_block = fs->block_size / d->sector_size;

    fs->num_groups =
        (fs->inodes_count + fs->inodes_per_group - 1) / fs->inodes_per_group;

    uint32_t gdt_block = (fs->block_size == 1024) ? 2 : 1;

    uint32_t gdt_bytes = fs->num_groups * sizeof(struct ext2_group_desc);
    uint32_t gdt_blocks = (gdt_bytes + fs->block_size - 1) / fs->block_size;

    fs->group_desc = kmalloc(gdt_blocks * fs->block_size);
    if (!fs->group_desc)
        return false;

    if (!block_read(fs->drive, gdt_block * fs->sectors_per_block,
                    (uint8_t *) fs->group_desc,
                    gdt_blocks * fs->sectors_per_block)) {
        kfree(fs->group_desc, gdt_blocks * fs->block_size);
        return false;
    }

    return true;
}

struct ext2_inode *ext2_path_lookup(struct ext2_fs *fs, struct ext2_inode *node,
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

    size_t len = path - start;
    char next_dir[len + 1];
    memcpy(next_dir, start, len);
    next_dir[len] = '\0';
    struct ext2_inode *next = ext2_find_file_in_dir(fs, node, -1, next_dir);

    if (!next) {
        k_printf("Did not find %s\n", next_dir);
        return NULL;
    }

    return ext2_path_lookup(fs, next, path);
}

void ext2_test(struct ide_drive *d, struct ext2_sblock *sblock) {
    struct ext2_fs fs;
    if (!ext2_mount(d, &fs, sblock)) {
        return;
    }

    struct ext2_inode root_inode;
    if (!ext2_read_inode(&fs, EXT2_ROOT_INODE, &root_inode)) {
        return;
    }

    uint32_t inode_num = ext2_alloc_inode(&fs);
    if (inode_num == (uint32_t) -1) {
        return;
    }

    struct ext2_inode inode = {0};
    inode.mode = EXT2_S_IFREG | 0644;
    inode.uid = 0;
    inode.gid = 0;
    inode.size = 0;
    inode.links_count = 1;
    inode.blocks = 0;
    uint32_t now = 563824800;
    inode.atime = now;
    inode.ctime = now;
    inode.mtime = now;
    inode.dtime = 0;
    if (!ext2_write_inode(&fs, inode_num, &inode)) {
        return;
    }
    ext2_link_file(&fs, &root_inode, EXT2_ROOT_INODE, inode_num, "fragmentation");

    k_printf("Created inode number: %u\n", inode_num);
}
