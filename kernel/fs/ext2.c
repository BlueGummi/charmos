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

struct k_full_inode *ext2_path_lookup(struct ext2_fs *fs,
                                      struct k_full_inode *node,
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

    struct k_full_inode *next = ext2_find_file_in_dir(fs, node, next_dir);

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

    struct ext2_inode r;
    if (!ext2_read_inode(&fs, EXT2_ROOT_INODE, &r)) {
        return;
    }

    struct k_full_inode root_inode = {.node = r, .inode_num = EXT2_ROOT_INODE};

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
    uint32_t now = 563824800; // was that the bite of '87?
    inode.atime = now;
    inode.ctime = now;
    inode.mtime = now;
    inode.dtime = 0;
    if (!ext2_write_inode(&fs, inode_num, &inode)) {
        return;
    }

    struct k_full_inode *i = ext2_path_lookup(&fs, &root_inode, "/k");
    if (i) {
        char *data =
            "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
            "Curabitur ornare vulputate purus, ut condimentum mauris "
            "mattis ac. Nullam ut nisi lectus. Mauris luctus congue "
            "turpis ac aliquet. Phasellus congue velit sed magna "
            "aliquam, quis imperdiet felis bibendum. Cras justo "
            "ligula, pulvinar eget luctus egestas, laoreet id sem. "
            "Nulla sed ultrices metus, ultricies lobortis massa. Nunc "
            "tincidunt lobortis purus, hendrerit bibendum orci euismod "
            "vitae. Class aptent taciti sociosqu ad litora torquent "
            "per conubia nostra, per inceptos himenaeos. Morbi varius "
            "ornare orci nec imperdiet. Integer consectetur ultrices "
            "tellus, non vestibulum neque aliquam non. Nullam non nisl "
            "nec nulla lacinia euismod pretium a massa. Vestibulum "
            "ante ipsum primis in faucibus orci luctus et ultrices "
            "posuere cubilia curae; Quisque suscipit tempor tellus sit "
            "amet venenatis.";

        k_printf("B size is %u\n", fs.block_size);
        ext2_write_file(&fs, i, 0, (uint8_t *) data, strlen(data));
        ext2_print_inode(i);
        ext2_dump_file_data(&fs, &i->node, 0, strlen(data));
        k_printf("Created inode number: %u\n", inode_num);
    } else {
        k_printf("didnt find k\n");
    }
}
