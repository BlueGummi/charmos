#include <alloc.h>
#include <errno.h>
#include <fs/ext2.h>
#include <fs/ext2_print.h>
#include <printf.h>
#include <stdint.h>
#include <string.h>

// TODO: Implement errno.h return codes or custom return codes

uint64_t PTRS_PER_BLOCK;

bool ext2_read_superblock(struct ide_drive *d, uint32_t partition_start_lba,
                          struct ext2_sblock *sblock) {
    uint8_t buffer[d->sector_size];
    uint32_t superblock_lba =
        partition_start_lba + (EXT2_SUPERBLOCK_OFFSET / d->sector_size);
    uint32_t superblock_offset = EXT2_SUPERBLOCK_OFFSET % d->sector_size;

    if (!ide_read_sector(d, superblock_lba, buffer)) {
        return false;
    }

    memcpy(sblock, buffer + superblock_offset, sizeof(struct ext2_sblock));

    return (sblock->magic == 0xEF53);
}

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

enum errno ext2_mount(struct ide_drive *d, struct ext2_fs *fs,
                struct ext2_sblock *sblock) {
    if (!fs || !sblock)
        return ERR_INVAL;

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
        return ERR_NO_MEM;

    if (!block_read(fs->drive, gdt_block * fs->sectors_per_block,
                    (uint8_t *) fs->group_desc,
                    gdt_blocks * fs->sectors_per_block)) {
        kfree(fs->group_desc);
        return ERR_FS_INTERNAL;
    }

    return ERR_OK;
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
    if (ERR_IS_FATAL(ext2_mount(d, &fs, sblock))) {
        return;
    }

    struct ext2_inode r;
    if (!ext2_read_inode(&fs, EXT2_ROOT_INODE, &r)) {
        return;
    }

    struct k_full_inode root_inode = {.node = r, .inode_num = EXT2_ROOT_INODE};
    /*
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
        struct k_full_inode i;
        i.node = inode;
        i.inode_num = inode_num;
        if (!ext2_write_inode(&fs, inode_num, &inode)) {
            return;
        }
    */
    char *data =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Donec lorem "
        "sapien, sollicitudin vitae leo ac, malesuada euismod erat. Fusce "
        "hendrerit sapien nec nulla scelerisque, vitae tempor lorem pretium. "
        "Vivamus egestas nulla ac leo dignissim, nec rutrum elit ullamcorper. "
        "Cras id suscipit arcu. Nulla at pharetra erat. Cras leo lectus, "
        "imperdiet id dapibus eu, venenatis et velit. Nulla dignissim pulvinar "
        "dui, eget condimentum nisi tincidunt ac. Fusce at leo porttitor elit "
        "scelerisque accumsan id ac dolor.\n\nIn sed consequat leo. Aenean "
        "porta augue elit, vitae rutrum nisi dictum placerat. Donec faucibus "
        "id est consequat viverra. Aenean fringilla dolor vel ipsum commodo "
        "feugiat. Sed faucibus lobortis metus eget iaculis. Morbi ac dui "
        "aliquam, vehicula nibh sed, lacinia felis. Ut eu sodales magna. "
        "Quisque bibendum risus ac sodales porta. Aenean tempor molestie felis "
        "at laoreet. Pellentesque at ligula quam. Proin vitae nulla ac odio "
        "accumsan rutrum. Fusce vulputate lectus sit amet enim vulputate "
        "fringilla. Nullam ac lectus lacinia, dignissim leo ac, sollicitudin "
        "lorem. Nam consectetur diam libero, id euismod felis auctor "
        "non.\n\nDuis laoreet pretium nulla ac elementum. Donec vel diam "
        "ipsum. Etiam ac erat sem. Praesent tempus nunc ex, eget viverra mi "
        "auctor efficitur. Suspendisse id cursus sapien. Aenean accumsan dolor "
        "ac arcu semper malesuada. Mauris hendrerit, turpis et finibus "
        "ullamcorper, leo nisi sollicitudin mauris, in commodo mauris mauris "
        "in felis. Proin vel facilisis eros. Integer luctus urna vel eros "
        "interdum, vel maximus quam volutpat.\n\nIn in suscipit leo. Cras "
        "varius nulla quis venenatis varius. Integer finibus elementum "
        "egestas. Suspendisse odio velit, bibendum a purus in, lobortis "
        "vestibulum elit. Sed ultricies metus eu orci pharetra placerat. Etiam "
        "iaculis cursus mi sed commodo. Duis at mi sed lacus bibendum eleifend "
        "eget vitae risus. Cras auctor pulvinar dictum. Fusce et finibus "
        "lorem, sit amet sodales ante. Morbi sed tristique lorem, ac fringilla "
        "libero.\n\nVestibulum tincidunt tincidunt maximus. Maecenas sagittis "
        "laoreet sapien, eget efficitur nisi mattis eu. Morbi arcu risus, "
        "congue venenatis posuere in, feugiat ut sapien. Donec urna arcu, "
        "aliquet in interdum in, vulputate nec nibh. Ut eu diam et ante "
        "lacinia scelerisque. Vivamus posuere egestas nunc. Mauris a convallis "
        "arcu. Fusce et accumsan augue. In sed sapien in lectus hendrerit "
        "commodo. Nullam non finibus quam. Duis vel neque ut felis convallis "
        "blandit et nec turpis. Curabitur vel pellentesque justo, at euismod "
        "erat. Quisque luctus mi erat, a interdum quam sodales non. Morbi "
        "consectetur dui nec erat interdum feugiat.\n\nSed eget nunc nunc. "
        "Etiam eget est ut eros vestibulum sodales eu a diam. Curabitur luctus "
        "sapien neque, ut molestie ex tincidunt quis. Nunc ultricies molestie "
        "diam. Sed vitae orci sed nisi suscipit fermentum in a turpis. Vivamus "
        "rhoncus odio nisi, fermentum facilisis lorem molestie nec. Donec quis "
        "lacinia felis, a vestibulum dolor.\n\nAenean ac lacus at nunc "
        "facilisis dignissim. Integer ac ex condimentum, viverra velit id, "
        "laoreet risus. Sed porta lobortis lorem, non posuere elit rutrum ut. "
        "Aliquam sit amet pellentesque tortor, nec lobortis diam. Nam tempus "
        "mollis eros eu vulputate. Pellentesque condimentum, turpis dictum "
        "fringilla aliquet, urna est consequat dolor, non vulputate lectus "
        "enim id neque. Integer gravida felis orci, quis cursus risus placerat "
        "in.\n\nVestibulum semper pellentesque ligula. Vestibulum convallis "
        "dolor sed scelerisque ullamcorper. Nullam bibendum imperdiet tortor "
        "vel euismod. Cras ut risus vel ex vehicula porta. Donec in tellus "
        "arcu. Vivamus maximus congue libero vel ornare. Quisque vel molestie "
        "tellus. Phasellus vel semper dolor, eu hendrerit ex.\n\nQuisque "
        "congue sodales lorem sed facilisis. Praesent fringilla tellus quis "
        "dapibus semper. Praesent ac odio eget ligula molestie dignissim. "
        "Quisque porttitor gravida urna, id aliquet velit condimentum "
        "vulputate. Ut sagittis tristique auctor. Nam placerat, tortor at "
        "facilisis suscipit, felis dui blandit odio, id varius metus leo a "
        "elit. Nullam sed tellus gravida, pulvinar erat eget, varius metus. "
        "Proin laoreet elementum dui, at molestie dolor. Praesent lacinia "
        "fringilla dui eget pharetra. Sed ipsum ligula, laoreet eu iaculis "
        "vitae, convallis non dui. Proin eu mollis dui. Proin at malesuada ex. "
        "Cras in ex eu leo facilisis ultrices ac facilisis dolor. Praesent ut "
        "luctus odio, nec suscipit ante.\n\nPhasellus tristique ultricies "
        "diam. Pellentesque tincidunt ex quis lacus auctor, at hendrerit felis "
        "lobortis. Morbi a porttitor magna. Integer ante est, convallis at "
        "interdum vitae, rutrum in urna. Sed vel ante lectus. Praesent vel "
        "metus eget odio egestas efficitur. Donec eu diam eget ante mattis "
        "auctor non in lorem. Phasellus eu pellentesque leo, ultricies semper "
        "nisl. In vitae turpis ante. In tincidunt, orci et malesuada viverra, "
        "leo libero dignissim nisl, a hendrerit urna nulla quis orci. Aliquam "
        "vehicula leo tempor nunc faucibus, eu semper ante euismod. Nam "
        "ultricies ante tortor. Vestibulum ut justo nunc. Nunc tristique "
        "dictum pulvinar.\n\n";

    //    ext2_link_file(&fs, &root_inode, &i, "file");
    ext2_symlink_file(&fs, &root_inode, "fold", "./lost+found");
    struct k_full_inode *n = ext2_path_lookup(&fs, &root_inode, "file");
    ext2_print_inode(&root_inode);
    //    ext2_write_file(&fs, &i, 0, (uint8_t *) data, strlen(data));
    //    ext2_dump_file_data(&fs, &i.node, 0, strlen(data));
}
