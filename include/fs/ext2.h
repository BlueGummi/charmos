#include <fs/generic.h>
#include <errno.h>
#include <fs/bcache.h>
#include <fs/vfs.h>
#include <spin_lock.h>
#include <stdint.h>

extern uint64_t PTRS_PER_BLOCK;

#define EXT2_NBLOCKS 15
#define EXT2_SUPERBLOCK_OFFSET 1024
#define EXT2_SIGNATURE_OFFSET 0x38
#define EXT2_SIGNATURE 0xEF53
#define EXT2_NAME_LEN 255
#define EXT2_ROOT_INODE 2
#define EXT2_S_IFSOCK 0xC000     // socket
#define EXT2_S_IFLNK 0xA000      // symbolic link
#define EXT2_S_IFREG 0x8000      // regular file
#define EXT2_S_IFBLK 0x6000      // block device
#define EXT2_S_IFDIR 0x4000      // directory
#define EXT2_S_IFCHR 0x2000      // character device
#define EXT2_S_IFIFO 0x1000      // FIFO
#define EXT2_S_IFMT 0xF000       // mask to extract file type from i_mode
#define EXT2_S_PERMS 0x0FFF      // lower 12 bits: special + rwx bits
#define EXT2_S_IRWXU 0x01C0      // owner permissions - ALL
#define EXT2_S_IRWXG 0x0038      // group permissions - ALL
#define EXT2_S_IRWXO 0x0007      // others permissions - ALL
#define EXT2_S_PERMS_ONLY 0x01FF // rwx bits only

#define EXT2_S_IRUSR 0x0100 // Owner can read
#define EXT2_S_IWUSR 0x0080 // Owner can write
#define EXT2_S_IXUSR 0x0040 // Owner can execute
#define EXT2_S_IRGRP 0x0020 // Group can read
#define EXT2_S_IWGRP 0x0010 // Group can write
#define EXT2_S_IXGRP 0x0008 // Group can execute
#define EXT2_S_IROTH 0x0004 // Others can read
#define EXT2_S_IWOTH 0x0002 // Others can write
#define EXT2_S_IXOTH 0x0001 // Others can execute

#define EXT2_FT_UNKNOWN 0  // Unknown file type
#define EXT2_FT_REG_FILE 1 // Regular file
#define EXT2_FT_DIR 2      // Directory
#define EXT2_FT_CHRDEV 3   // Character device
#define EXT2_FT_BLKDEV 4   // Block device
#define EXT2_FT_FIFO 5     // FIFO (named pipe)
#define EXT2_FT_SOCK 6     // Unix domain socket
#define EXT2_FT_SYMLINK 7  // Symbolic link
#define EXT2_FT_MAX 8      // Number of defined file types

// ext2 inode flags
#define EXT2_SECRM_FL 0x00000001     // Secure deletion
#define EXT2_UNRM_FL 0x00000002      // Undelete
#define EXT2_COMPR_FL 0x00000004     // Compress file
#define EXT2_SYNC_FL 0x00000008      // Synchronous updates
#define EXT2_IMMUTABLE_FL 0x00000010 // Immutable file
#define EXT2_APPEND_FL 0x00000020    // Writes only append
#define EXT2_NODUMP_FL 0x00000040    // Don't include in backups
#define EXT2_NOATIME_FL 0x00000080   // Don't update access time

#define EXT2_DIRTY_FL 0x00000100        // Dirty (compress support)
#define EXT2_COMPRBLK_FL 0x00000200     // One or more compressed clusters
#define EXT2_NOCOMPR_FL 0x00000400      // Don't compress
#define EXT2_ECOMPR_FL 0x00000800       // Compression error
#define EXT2_IMAGIC_FL 0x00002000       // AFS directory
#define EXT2_JOURNAL_DATA_FL 0x00004000 // Write data to journal (data=journal)

#define EXT2_NOTAIL_FL 0x00008000    // File tail not merged (reiserfs)
#define EXT2_DIRSYNC_FL 0x00010000   // Directory sync updates
#define EXT2_TOPDIR_FL 0x00020000    // Top of directory hierarchy
#define EXT2_HUGE_FILE_FL 0x00040000 // Set to each huge file
#define EXT2_EXTENTS_FL 0x00080000   // Inode uses extents

#define EXT2_EA_INODE_FL 0x00200000    // Inode stores extended attributes
#define EXT2_INLINE_DATA_FL 0x10000000 // Data is stored inline in inode
#define EXT2_PROJINHERIT_FL 0x20000000 // Project ID inheritance

#define EXT2_RESERVED_FL 0x80000000 // Reserved for ext3/4

// Useful masks
#define EXT2_FL_USER_VISIBLE 0x000BDFFF // User-visible flags
#define EXT2_FL_USER_MODIFIABLE                                                \
    (EXT2_FL_USER_VISIBLE & ~(EXT2_SECRM_FL | EXT2_UNRM_FL))

#define MIN(x, y) ((x > y) ? y : x)

#define MAKE_NOP_CALLBACK                                                      \
    static bool nop_callback(struct ext2_fs *fs, struct ext2_dir_entry *entry, \
                             void *ctx_ptr, uint32_t block_num,                \
                             uint32_t entry_num, uint32_t entry_offset) {      \
        (void) fs;                                                             \
        (void) entry_offset;                                                   \
        (void) entry;                                                          \
        (void) ctx_ptr;                                                        \
        (void) block_num;                                                      \
        (void) entry_num;                                                      \
        return false;                                                          \
    }

struct ext2_sblock {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t r_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
    char last_mounted[64];
    uint32_t algorithm_usage_bitmap;
    uint8_t prealloc_blocks;
    uint8_t prealloc_dir_blocks;
    uint16_t padding;

    union {
        struct {
            uint32_t journal_uuid[4];
            uint32_t journal_inum;
            uint32_t journal_dev;
            uint32_t last_orphan;
            uint32_t hash_seed[4];
            uint8_t def_hash_version;
            uint8_t journal_backup_type;
            uint16_t desc_size;
            uint32_t default_mount_opts;
            uint32_t first_meta_bg;
            uint32_t mkfs_time;
            uint32_t journal_blocks[17];
            uint32_t quota_group_inode;   // [4] → offset 0x258
            uint32_t quota_project_inode; // [5] → offset 0x25C
        };
        uint32_t reserved[204];
    };
} __attribute__((packed));

struct ext2_group_desc {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint32_t reserved[3];
} __attribute__((__packed__));

struct ext2_inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
    uint32_t osd1;

    uint32_t block[EXT2_NBLOCKS];
    uint32_t generation;
    uint32_t file_acl;
    uint32_t dir_acl;
    uint32_t faddr;
    uint8_t frag[16];
    uint8_t osd2[12];
} __attribute__((packed));

struct ext2_full_inode {
    struct ext2_inode node;
    uint32_t inode_num;
    struct spinlock lock;
};

struct ext2_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[EXT2_NAME_LEN + 1];
} __attribute__((packed));

struct ext2_fs {
    struct generic_partition *partition;
    struct generic_disk *drive;
    struct ext2_sblock *sblock;
    struct ext2_group_desc *group_desc;
    struct bcache_entry *sbcache_ent;
    struct bcache_entry *gdesc_cache_ent;
    uint32_t num_groups;
    uint32_t inodes_count;
    uint32_t inodes_per_group;
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint16_t inode_size;

    /* lock the fs struct */
    struct spinlock lock;
};

typedef bool (*dir_entry_callback)(struct ext2_fs *fs,
                                   struct ext2_dir_entry *entry, void *ctx,
                                   uint32_t block_num, uint32_t entry_num,
                                   uint32_t entry_offset);

typedef void (*ext2_block_visitor)(struct ext2_fs *fs, struct ext2_inode *inode,
                                   uint32_t depth, uint32_t *block_ptr,
                                   void *user_data);

//
//
// R/W + Mount
//
//

struct bcache_entry *ext2_block_read(struct ext2_fs *fs, uint32_t block_num);

bool ext2_block_write(struct ext2_fs *fs, struct bcache_entry *ent);

bool ext2_read_superblock(struct generic_partition *,
                          struct ext2_sblock *sblock);

bool ext2_write_superblock(struct ext2_fs *fs);
bool ext2_write_group_desc(struct ext2_fs *fs);

enum errno ext2_mount(struct generic_partition *, struct ext2_fs *fs,
                      struct ext2_sblock *sblock, struct vfs_node *out_node);

struct vfs_node *ext2_g_mount(struct generic_partition *);

struct bcache_entry *ext2_inode_read(struct ext2_fs *fs, uint32_t inode_idx,
                                     struct ext2_inode *inode_out);

bool ext2_inode_write(struct ext2_fs *fs, uint32_t inode_num,
                      const struct ext2_inode *inode);

uint32_t ext2_get_or_set_block(struct ext2_fs *fs, struct ext2_inode *inode,
                               uint32_t block_index, uint32_t new_block_num,
                               bool allocate, bool *was_allocated);

//
//
// Util
//
//

uint32_t ext2_block_to_lba(struct ext2_fs *fs, uint32_t block_num);
bool ext2_dirent_valid(struct ext2_dir_entry *entry);
void ext2_init_inode(struct ext2_inode *new_inode, uint16_t mode);

void ext2_init_dirent(struct ext2_fs *fs, struct ext2_dir_entry *new_entry,
                      uint32_t inode_num, const char *name, uint8_t type);

uint8_t ext2_extract_ftype(uint16_t mode);
uint32_t ext2_get_inode_group(struct ext2_fs *fs, uint32_t inode);
uint32_t ext2_get_block_group(struct ext2_fs *fs, uint32_t block);
bool ext2_fs_lock(struct ext2_fs *fs);
void ext2_fs_unlock(struct ext2_fs *fs, bool i);
void ext2_prefetch_block(struct ext2_fs *fs, uint32_t block);
struct bcache_entry *ext2_create_bcache_ent(struct ext2_fs *fs,
                                            uint32_t block);

//
//
// Higher level stuff
//
//

enum errno ext2_link_file(struct ext2_fs *fs, struct ext2_full_inode *dir_inode,
                          struct ext2_full_inode *inode, const char *name,
                          uint8_t type, bool increment_links);

enum errno ext2_unlink_file(struct ext2_fs *fs,
                            struct ext2_full_inode *dir_inode, const char *name,
                            bool free_blocks, bool decrement_links);

enum errno ext2_create_file(struct ext2_fs *fs,
                            struct ext2_full_inode *parent_dir,
                            const char *name, uint16_t mode,
                            bool increment_links);

enum errno ext2_symlink_file(struct ext2_fs *fs,
                             struct ext2_full_inode *dir_inode,
                             const char *name, const char *target);

enum errno ext2_write_file(struct ext2_fs *fs, struct ext2_full_inode *inode,
                           uint32_t offset, const uint8_t *src, uint32_t size);

enum errno ext2_read_file(struct ext2_fs *fs, struct ext2_full_inode *inode,
                          uint32_t offset, uint8_t *buffer, uint64_t length);

enum errno ext2_truncate_file(struct ext2_fs *fs, struct ext2_full_inode *inode,
                              uint32_t new_size);

enum errno ext2_chmod(struct ext2_fs *fs, struct ext2_full_inode *node,
                      uint16_t new_mode);

enum errno ext2_chown(struct ext2_fs *fs, struct ext2_full_inode *node,
                      uint32_t new_uid, uint32_t new_gid);

enum errno ext2_readlink(struct ext2_fs *fs, struct ext2_full_inode *node,
                         char *buf, uint64_t size);

struct ext2_full_inode *ext2_find_file_in_dir(struct ext2_fs *fs,
                                              struct ext2_full_inode *dir_inode,
                                              const char *fname,
                                              uint8_t *type_out);

bool ext2_dir_contains_file(struct ext2_fs *fs,
                            struct ext2_full_inode *dir_inode,
                            const char *fname);

enum errno ext2_mkdir(struct ext2_fs *fs, struct ext2_full_inode *parent_dir,
                      const char *name, uint16_t mode);

enum errno ext2_rmdir(struct ext2_fs *fs, struct ext2_full_inode *parent_dir,
                      const char *name);

enum errno ext2_readdir(struct ext2_fs *fs, struct ext2_full_inode *dir_inode,
                        struct ext2_dir_entry *out, uint32_t entry_offset);

//
//
//
// Alloc/dealloc
//
//

uint32_t ext2_alloc_block(struct ext2_fs *fs);
bool ext2_free_block(struct ext2_fs *fs, uint32_t block_num);
uint32_t ext2_alloc_inode(struct ext2_fs *fs);
bool ext2_free_inode(struct ext2_fs *fs, uint32_t inode_num);

bool ext2_walk_dir(struct ext2_fs *fs, struct ext2_full_inode *dir_inode,
                   dir_entry_callback cb, void *ctx, bool ff_avail);

void ext2_traverse_inode_blocks(struct ext2_fs *fs, struct ext2_inode *inode,
                                ext2_block_visitor visitor, void *user_data,
                                bool readahead);

void ext2_g_print(struct generic_partition *);

void ext2_print_superblock(struct ext2_sblock *sblock);

void ext2_print_inode(const struct ext2_full_inode *node);

void ext2_dump_file_data(struct ext2_fs *fs, const struct ext2_inode *inode,
                         uint32_t start_block_index, uint32_t length);

#pragma once
