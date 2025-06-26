#include <fs/ext2.h>
#include <fs/generic.h>

struct fs_cache_entry *ext2_icache_get(struct ext2_fs *fs, uint64_t inode_num) {
    return fs_cache_get(fs->inode_cache, inode_num);
}

struct fs_cache_entry *ext2_bcache_get(struct ext2_fs *fs, uint64_t block_num) {
    return fs_cache_get(fs->block_cache, block_num);
}

bool ext2_icache_insert(struct ext2_fs *fs, uint64_t inode_num,
                        struct fs_cache_entry *ent) {
    return fs_cache_insert(fs->inode_cache, inode_num, ent);
}

bool ext2_bcache_insert(struct ext2_fs *fs, uint64_t block_num,
                        struct fs_cache_entry *ent) {
    return fs_cache_insert(fs->block_cache, block_num, ent);
}


