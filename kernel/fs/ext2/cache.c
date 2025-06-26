#include <fs/ext2.h>
#include <fs/generic.h>
#include <mem/alloc.h>

struct fs_cache_entry *ext2_bcache_get(struct ext2_fs *fs, uint64_t block_num) {
    return fs_cache_get(fs->block_cache, block_num);
}

bool ext2_bcache_insert(struct ext2_fs *fs, uint64_t block_num,
                        struct fs_cache_entry *ent) {
    return fs_cache_insert(fs->block_cache, block_num, ent);
}

bool ext2_bcache_evict(struct ext2_fs *fs) {
    return fs_cache_evict(fs->block_cache);
}

struct fs_cache_entry *
ext2_bcache_ent_create(struct ext2_fs *fs, uint64_t block_num, bool no_evict) {
    struct generic_partition *p = fs->partition;
    struct generic_disk *d = fs->drive;

    uint32_t base_lba = block_num * fs->sectors_per_block;
    uint32_t lba = base_lba + p->start_lba;
    uint32_t spb = fs->sectors_per_block;

    uint8_t *buf = kmalloc(fs->block_size);

    if (!d->read_sector(d, lba, buf, spb)) {
        return NULL;
    }

    struct fs_cache_entry *entry = kzalloc(sizeof(struct fs_cache_entry));
    entry->buffer = buf;
    entry->number = block_num;
    entry->size = fs->block_size;
    entry->no_evict = no_evict;
    ext2_bcache_insert(fs, block_num, entry);
    return entry;
}
