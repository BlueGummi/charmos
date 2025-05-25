#include <disk.h>
#include <fs/ext2.h>
#include <print.h>
#include <printf.h>
#include <stdint.h>
#include <string.h>
#include <vmalloc.h>

uint64_t PTRS_PER_BLOCK;

void ext2_print_inode(const struct ext2_inode *inode);

bool block_read(struct ide_drive *d, uint32_t lba, uint8_t *buffer, uint32_t sector_count) {
    if (!buffer)
        return false;

    for (uint32_t i = 0; i < sector_count; ++i) {
        if (!ide_read_sector(d, lba + i, buffer + (i * 512))) {
            return false;
        }
    }
    return true;
}

bool ext2_mount(struct ide_drive *d, struct ext2_fs *fs, struct ext2_sblock *sblock) {
    if (!fs || !sblock)
        return false;

    fs->drive = d;
    fs->sblock = sblock;
    fs->inodes_count = sblock->inodes_count;
    fs->inodes_per_group = sblock->inodes_per_group;
    fs->inode_size = sblock->inode_size;
    fs->block_size = 1024 << sblock->log_block_size;
    fs->sectors_per_block = fs->block_size / 512;

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

bool read_block_ptrs(struct ext2_fs *fs, uint32_t block_num, uint32_t *buf) {
    if (!fs || !buf)
        return false;

    uint32_t lba = block_num * fs->sectors_per_block;
    return block_read(fs->drive, lba, (uint8_t *) buf, fs->sectors_per_block);
}

bool ext2_read_inode(struct ext2_fs *fs, uint32_t inode_idx,
                     struct ext2_inode *inode_out) {
    if (!fs || !inode_out || inode_idx == 0)
        return false;

    uint32_t inodes_per_group = fs->sblock->inodes_per_group;
    uint32_t inode_size = fs->sblock->inode_size;

    uint32_t group = (inode_idx - 1) / inodes_per_group;
    uint32_t index_in_group = (inode_idx - 1) % inodes_per_group;

    struct ext2_group_desc *desc = &fs->group_desc[group];
    uint32_t inode_table_block = desc->inode_table;

    uint32_t offset_bytes = index_in_group * inode_size;
    uint32_t block_offset = offset_bytes / fs->block_size;
    uint32_t offset_in_block = offset_bytes % fs->block_size;

    uint32_t inode_block_num = inode_table_block + block_offset;
    uint32_t inode_lba = inode_block_num * fs->sectors_per_block;

    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf)
        return false;

    if (!block_read(fs->drive, inode_lba, buf, fs->sectors_per_block)) {
        kfree(buf, fs->block_size);
        return false;
    }

    memcpy(inode_out, buf + offset_in_block, sizeof(struct ext2_inode));
    kfree(buf, fs->block_size);
    return true;
}

static bool search_dir_block(struct ext2_fs *fs, uint32_t block_num,
                             const char *fname, struct ext2_inode **out_node) {
    if (!fs || !fname || !out_node) {
        return false;
    }

    uint8_t dir_buf[fs->block_size];
    uint32_t dir_lba = block_num * fs->sectors_per_block;

    if (!block_read(fs->drive, dir_lba, dir_buf, fs->sectors_per_block))
        return false;

    uint32_t offset = 0;
    while (offset < fs->block_size) {
        struct ext2_dir_entry *entry =
            (struct ext2_dir_entry *) (dir_buf + offset);

        if (entry->rec_len < 8 || entry->rec_len + offset > fs->block_size)
            break;

        if (entry->inode != 0 &&
            memcmp(entry->name, fname, entry->name_len) == 0 &&
            fname[entry->name_len] == '\0') {

            k_printf("got '%s' at %lu\n", entry->name, entry->inode);

            *out_node = kmalloc(sizeof(struct ext2_inode));
            ext2_read_inode(fs, entry->inode, *out_node);
            return true;
        }

        offset += entry->rec_len;
    }

    return false;
}

struct ext2_inode *ext2_find_file_in_dir(struct ext2_fs *fs,
                                         const struct ext2_inode *dir_inode,
                                         const char *fname) {
    if (!fs || !dir_inode || !fname) {
        return NULL;
    }

    struct ext2_inode *found = NULL;

    // Direct blocks (0..11)
    for (uint32_t i = 0; i < 12; ++i) {
        if (dir_inode->block[i] != 0 &&
            search_dir_block(fs, dir_inode->block[i], fname, &found))
            return found;
    }

    // Singly indirect
    if (dir_inode->block[12]) {
        uint32_t ptrs[PTRS_PER_BLOCK];
        if (read_block_ptrs(fs, dir_inode->block[12], ptrs)) {
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (ptrs[i] != 0 &&
                    search_dir_block(fs, ptrs[i], fname, &found))
                    return found;
            }
        }
    }

    // Doubly indirect
    if (dir_inode->block[13]) {
        uint32_t level1[PTRS_PER_BLOCK];
        if (read_block_ptrs(fs, dir_inode->block[13], level1)) {
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (level1[i] == 0)
                    continue;

                uint32_t level2[PTRS_PER_BLOCK];
                if (read_block_ptrs(fs, level1[i], level2)) {
                    for (uint32_t j = 0; j < PTRS_PER_BLOCK; ++j) {
                        if (level2[j] != 0 &&
                            search_dir_block(fs, level2[j], fname, &found))
                            return found;
                    }
                }
            }
        }
    }

    // Triply indirect
    if (dir_inode->block[14]) {
        uint32_t level1[PTRS_PER_BLOCK];
        if (read_block_ptrs(fs, dir_inode->block[14], level1)) {
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (level1[i] == 0)
                    continue;

                uint32_t level2[PTRS_PER_BLOCK];
                if (read_block_ptrs(fs, level1[i], level2)) {
                    for (uint32_t j = 0; j < PTRS_PER_BLOCK; ++j) {
                        if (level2[j] == 0)
                            continue;

                        uint32_t level3[PTRS_PER_BLOCK];
                        if (read_block_ptrs(fs, level2[j], level3)) {
                            for (uint32_t k = 0; k < PTRS_PER_BLOCK; ++k) {
                                if (level3[k] != 0 &&
                                    search_dir_block(fs, level3[k], fname,
                                                     &found))
                                    return found;
                            }
                        }
                    }
                }
            }
        }
    }

    return NULL;
}

void ext2_print_inode(const struct ext2_inode *inode) {
    if (!inode)
        return;

    k_printf("Inode Information:\n");
    k_printf("  Mode: 0x%04x\n", inode->mode);
    k_printf("  UID: %u\n", inode->uid);
    k_printf("  GID: %u\n", inode->gid);
    k_printf("  Size: %u bytes\n", inode->size);
    k_printf("  Access Time: ");
    ptime(inode->atime);
    k_printf("  Creation Time: ");
    ptime(inode->ctime);
    k_printf("  Modification Time: ");
    ptime(inode->mtime);
    k_printf("  Deletion Time: ");
    ptime(inode->dtime);
    k_printf("  Links Count: %u\n", inode->links_count);
    k_printf("  Blocks (512-byte units): %u\n", inode->blocks);
    k_printf("  Flags: 0x%08x\n", inode->flags);
    k_printf("  osd1: 0x%08x\n", inode->osd1);

    k_printf("  Block Pointers:\n");
    for (int i = 0; i < EXT2_NBLOCKS; ++i) {
        k_printf("    [%-2d] = %u\n", i, inode->block[i]);
    }

    k_printf("  Generation: %u\n", inode->generation);
    k_printf("  File ACL: %u\n", inode->file_acl);
    k_printf("  Directory ACL: %u\n", inode->dir_acl);
    k_printf("  Fragment Address: %u\n", inode->faddr);

    k_printf("  Fragment (raw): ");
    for (int i = 0; i < 16; ++i) {
        k_printf("%02x ", inode->frag[i]);
    }
    k_printf("\n");

    k_printf("  OS Specific 2: ");
    for (int i = 0; i < 12; ++i) {
        k_printf("%02x ", inode->osd2[i]);
    }
    k_printf("\n");
}

void ext2_dump_file_data(struct ext2_fs *fs, const struct ext2_inode *inode,
                         uint32_t start_block_index, uint32_t length) {
    if (!fs || !inode)
        return;

    uint32_t bytes_remaining = length;
    uint32_t block_size = fs->block_size;
    uint32_t block_index = start_block_index;

    while (block_index < 12 && bytes_remaining > 0) {
        uint32_t block_num = inode->block[block_index];
        if (block_num == 0) {
            k_printf("block %u is not allocated\n", block_index);
            break;
        }

        uint32_t lba = block_num * fs->sectors_per_block;
        uint8_t buf[block_size];
        if (!block_read(fs->drive, lba, buf, fs->sectors_per_block)) {
            k_printf("failed to read block %u (LBA %u)\n", block_num, lba);
            break;
        }

        uint32_t to_read =
            bytes_remaining < block_size ? bytes_remaining : block_size;

        for (uint32_t i = 0; i < to_read; ++i) {
            k_printf("%c", buf[i]);
        }

        bytes_remaining -= to_read;
        block_index++;
    }

    if (bytes_remaining > 0) {
        k_printf("\nrequested length exceeds direct blocks available\n");
    }
}

void print_ext2_sblock(struct ext2_sblock *sblock) {
    if (!sblock)
        return;

    k_printf("Ext2 superblock information:\n");
    k_printf("  Inodes Count: %u\n", sblock->inodes_count);
    k_printf("  Blocks Count: %u\n", sblock->blocks_count);
    k_printf("  Reserved Blocks Count: %u\n", sblock->r_blocks_count);
    k_printf("  Free Blocks Count: %u\n", sblock->free_blocks_count);
    k_printf("  Free Inodes Count: %u\n", sblock->free_inodes_count);
    k_printf("  First Data Block: %u\n", sblock->first_data_block);
    k_printf("  Log Block Size: %u\n", sblock->log_block_size);
    k_printf("  Log Fragment Size: %u\n", sblock->log_frag_size);
    k_printf("  Blocks per Group: %u\n", sblock->blocks_per_group);
    k_printf("  Fragments per Group: %u\n", sblock->frags_per_group);
    k_printf("  Inodes per Group: %u\n", sblock->inodes_per_group);
    k_printf("  Mount Time: ");
    ptime(sblock->mtime);
    k_printf("  Write Time: ");
    ptime(sblock->wtime);
    k_printf("  Mount Count: %u\n", sblock->mnt_count);
    k_printf("  Max Mount Count: %u\n", sblock->max_mnt_count);
    k_printf("  Magic: 0x%04x\n", sblock->magic);
    k_printf("  State: %s\n", sblock->state == 1 ? "OK" : "ERROR");
    char *error_recov = "UNKNOWN";
    switch (sblock->errors) {
    case 1: error_recov = "IGNORE"; break;
    case 2: error_recov = "REMOUNT AS RO"; break;
    case 3: error_recov = "PANIC"; break;
    }
    k_printf("  Error state: %s\n", error_recov);
    k_printf("  Minor Revision Level: %u\n", sblock->minor_rev_level);
    k_printf("  Last Check: ");
    ptime(sblock->lastcheck);
    k_printf("  Check Interval: %u\n", sblock->checkinterval);
    char *creator = "OTHER";
    switch (sblock->creator_os) {
    case 0: creator = "LINUX"; break;
    case 1: creator = "GNU HURD"; break;
    case 2: creator = "MASIX"; break;
    case 3: creator = "FREEBSD"; break;
    }
    k_printf("  Creator OS: %s\n", creator);
    k_printf("  Revision Level: %u\n", sblock->rev_level);
    k_printf("  Default Reserved UID: %u\n", sblock->def_resuid);
    k_printf("  Default Reserved GID: %u\n", sblock->def_resgid);
    if (sblock->rev_level >= 1) {
        k_printf("  First Inode: %u\n", sblock->first_ino);
        k_printf("  Inode Size: %u\n", sblock->inode_size);
        k_printf("  Block Group Number: %u\n", sblock->block_group_nr);
        k_printf("  Feature Compatibility: %u\n", sblock->feature_compat);
        k_printf("  Feature Incompatibility: %u\n", sblock->feature_incompat);
        k_printf("  Feature Read-Only Compatibility: %u\n",
                 sblock->feature_ro_compat);

        k_printf("  UUID: ");
        for (int i = 0; i < 16; i++) {
            k_printf("%02x", sblock->uuid[i]);
            if (i < 15)
                k_printf("-");
        }
        k_printf("\n");

        k_printf("  Volume Name: %-16s\n", sblock->volume_name);
        k_printf("  Last Mounted: %-64s\n", sblock->last_mounted);
        k_printf("  Algorithm Usage Bitmap: %u\n",
                 sblock->algorithm_usage_bitmap);
        k_printf("  Preallocated Blocks: %u\n", sblock->prealloc_blocks);
        k_printf("  Preallocated Directory Blocks: %u\n",
                 sblock->prealloc_dir_blocks);
        k_printf("  Padding: %u\n", sblock->padding);
    }
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
    struct ext2_inode *next = ext2_find_file_in_dir(fs, node, next_dir);
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

    PTRS_PER_BLOCK = (1024 << sblock->log_block_size) / 4;
    struct ext2_inode *node =
        ext2_path_lookup(&fs, &root_inode, "/crashout/rand.txt");
    ext2_print_inode(node);
    ext2_dump_file_data(&fs, node, 0, node->size);
}
