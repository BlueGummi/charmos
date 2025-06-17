#include <devices/generic_disk.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#pragma once

// TODO: flags on file creation

struct vfs_node;
struct vfs_mount;
extern char *g_root_part;
extern struct vfs_node *g_root_node;
extern struct vfs_mount *g_mount_list_head;

#define VFS_NAME_MAX 256 // this because of ext2

#define VFS_MODE_READ 0x0001     // Read permissions (all)
#define VFS_MODE_WRITE 0x0002    // Write permissions (all)
#define VFS_MODE_EXEC 0x0004     // Exec permissions (all)
#define VFS_MODE_DIR 0x4000      // Is directory
#define VFS_MODE_FILE 0x8000     // Is file
#define VFS_MODE_SYMLINK 0xA000  // Is symbolic link
#define VFS_MODE_CHARDEV 0x2000  // Is character device
#define VFS_MODE_BLOCKDEV 0x6000 // Is block device
#define VFS_MODE_PIPE 0x1000     // Is pipe
#define VFS_MODE_SOCKET 0xC000   // Is socket

#define VFS_MODE_G_READ 0x1000  // Read permissions (group)
#define VFS_MODE_G_WRITE 0x2000 // Write permissions (group)
#define VFS_MODE_G_EXEC 0x4000  // Exec permissions (group)

#define VFS_MODE_O_READ 0x010000  // Read permissions (owner)
#define VFS_MODE_O_WRITE 0x020000 // Write permissions (owner)
#define VFS_MODE_O_EXEC 0x040000  // Exec permissions (owner)

// clang-format off
enum vfs_node_flags : uint32_t {
    VFS_NODE_NONE        = 0x0000, // No flags
    VFS_NODE_MOUNTPOINT  = 0x0001, // This node is a mountpoint
    VFS_NODE_SYMLINK     = 0x0002, // This node is a symlink
    VFS_NODE_HIDDEN      = 0x0004, // Should be hidden from directory listings
    VFS_NODE_DEVICE      = 0x0008, // This node represents a device (char/block)
    VFS_NODE_PIPE        = 0x0010, // Pipe or FIFO
    VFS_NODE_SOCKET      = 0x0020, // Unix domain socket
    VFS_NODE_SYNC        = 0x0030,
    VFS_NODE_TEMPORARY   = 0x0040, // Temporary/in-memory (e.g., tmpfs)
    VFS_NODE_NOATIME     = 0x0080, // Access time updates disabled
    VFS_NODE_APPENDONLY  = 0x0100, // Only allows appending
    VFS_NODE_IMMUTABLE   = 0x0200, // Cannot be modified
    VFS_NODE_NOFOLLOW    = 0x0400, // Symlink should not be followed
    VFS_NODE_IN_USE      = 0x0800, // Open count > 0 (used internally)
    VFS_NODE_DIRSYNC     = 0x1000,
};
// clang-format on

enum vfs_open_opts : uint32_t {
    VFS_OPEN_READ = 0x01,  // Open for reading
    VFS_OPEN_WRITE = 0x02, // Open for writing
    VFS_OPEN_RDWR = VFS_OPEN_READ | VFS_OPEN_WRITE,

    VFS_OPEN_APPEND = 0x04,           // Writes go to end of file
    VFS_OPEN_CREAT = 0x08,            // Create if it doesn't exist
    VFS_OPEN_TRUNC = 0x10,            // Truncate to 0 size if it exists
    VFS_OPEN_EXCL = 0x20,             // Fail if file exists (with CREAT)
    VFS_OPEN_DIR = 0x40,              // Must be a directory
    VFS_OPEN_SYMLINK_NOFOLLOW = 0x80, // Don’t follow final symlink

    VFS_OPEN_NONBLOCK = 0x100, // Non-blocking I/O (device nodes/pipes)
    VFS_OPEN_SYNC = 0x200,     // Synchronous writes
    VFS_OPEN_NOATIME = 0x400,  // Don’t update access time
};

struct vfs_stat {
    uint16_t mode;
    uint64_t size;

    uint64_t inode; // inode number
    uint32_t nlink; // Link count

    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint16_t present_mask;
};

struct vfs_dirent {
    char name[VFS_NAME_MAX];
    uint64_t inode;
    uint16_t mode;
    void *dirent_data;
};

struct vfs_node;
struct vfs_ops {
    enum errno (*read)(struct vfs_node *node, void *buf, uint64_t size,
                       uint64_t offset); // read data from file

    enum errno (*write)(struct vfs_node *node, const void *buf, uint64_t size,
                        uint64_t offset); // write data to file

    enum errno (*open)(struct vfs_node *node,
                       uint32_t flags); // open file with flags

    enum errno (*close)(struct vfs_node *node); // close file

    enum errno (*create)(struct vfs_node *parent, const char *name,
                         uint16_t mode);

    enum errno (*mknod)(struct vfs_node *parent, const char *name,
                        uint16_t mode,
                        uint32_t dev); // special devices

    enum errno (*symlink)(struct vfs_node *parent, const char *target,
                          const char *link_name);

    enum errno (*mount)(struct vfs_node *mountpoint, struct vfs_node *target);

    enum errno (*unmount)(struct vfs_mount *mountpoint);

    enum errno (*stat)(struct vfs_node *node,
                       struct vfs_stat *out); // get file metadata

    enum errno (*readdir)(struct vfs_node *node, struct vfs_dirent *out,
                          uint64_t index); // read directory entry at index

    enum errno (*mkdir)(struct vfs_node *parent, const char *name,
                        uint16_t mode); // create directory

    enum errno (*rmdir)(struct vfs_node *parent,
                        const char *name); // remove directory

    enum errno (*unlink)(struct vfs_node *parent,
                         const char *name); // delete file

    enum errno (*rename)(struct vfs_node *old_parent, const char *old_name,
                         struct vfs_node *new_parent,
                         const char *new_name); // rename/move file or directory

    enum errno (*truncate)(struct vfs_node *node,
                           uint64_t length); // resize file to given length

    enum errno (*readlink)(struct vfs_node *node, char *buf,
                           uint64_t size); // read symlink target into buffer

    enum errno (*link)(struct vfs_node *parent, struct vfs_node *target,
                       const char *link_name); // create hard link to target

    enum errno (*chmod)(struct vfs_node *node,
                        uint16_t mode); // change file permissions

    enum errno (*chown)(struct vfs_node *node, uint32_t uid,
                        uint32_t gid); // change file ownership

    enum errno (*utime)(
        struct vfs_node *node, uint64_t atime,
        uint64_t mtime); // update access and modification times (unix time)

    void (*destroy)(struct vfs_node *node);

    // enum errno (*ioctl)(struct vfs_node *node, uint64_t request, void *arg);
    // // device-specific control (optional)

    struct vfs_node *(*finddir)(struct vfs_node *node,
                                const char *name); // find a child node by name
};

struct vfs_mount {
    struct vfs_node *mount_point;  /* vfs_node of the mountpoint */
    struct vfs_mount *mount_mount; /* mount representing mountpoint */
    struct vfs_node *root;         /* root of the mounted filesystem */
    struct vfs_ops *ops;           /* filesystem operation interface */
    char name[256];
    void *fs_data; /* optional filesystem driver data */
};

struct vfs_node {
    enum fs_type fs_type;  /* filesystem type */
    uint64_t open_handles; /* how many things have this open */
    char name[256];
    uint64_t unique_id; /* exclusively unique ID - one per node */
    uint32_t flags;
    uint16_t mode;
    uint64_t size;

    void *fs_data;                 /* optional filesystem driver data */
    void *fs_node_data;            /* optional filesystem driver data */
    struct vfs_mount *child_mount; /* NULL if no child is mounted */

    struct vfs_ops *ops;
};
void vfs_node_print(const struct vfs_node *node);
enum errno vfs_mount(struct vfs_node *mountpoint, struct vfs_node *target);
enum errno vfs_unmount(struct vfs_mount *mountpoint);
struct vfs_node *vfs_finddir(struct vfs_node *node, const char *fname);
