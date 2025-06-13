#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#pragma once

#define VFS_NAME_MAX 256 // this because of ext2

#define VFS_MODE_READ 0x0001
#define VFS_MODE_WRITE 0x0002
#define VFS_MODE_EXEC 0x0004
#define VFS_MODE_DIR 0x4000
#define VFS_MODE_FILE 0x8000
#define VFS_MODE_SYMLINK 0xA000

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
//clang-format on


enum vfs_node_type : uint32_t {
    VFS_UNKNOWN = 0x0,
    VFS_FILE = 0x01,
    VFS_DIR = 0x02,
    VFS_SYMLINK = 0x03,
    VFS_CHARDEV = 0x04,
    VFS_BLOCKDEV = 0x05,
    VFS_PIPE = 0x06,
    VFS_SOCKET = 0x07
};

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
    enum vfs_node_type type;
    uint64_t size;

    uint64_t inode; // inode number
    uint32_t mode;
    uint32_t nlink; // Link count

    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
};

struct vfs_dirent {
    char name[VFS_NAME_MAX];
    enum vfs_node_type type;
    uint64_t inode;
    void *dirent_data;
};

struct vfs_node;
struct vfs_ops {
    uint64_t (*read)(struct vfs_node *node, void *buf, uint64_t size,
                     uint64_t offset); // read data from file, returns # bytes read

    uint64_t (*write)(struct vfs_node *node, const void *buf, uint64_t size,
                      uint64_t offset); // write data to file, returns # bytes written

    enum errno (*open)(struct vfs_node *node, int flags); // open file with flags

    enum errno (*close)(struct vfs_node *node); // close file

    enum errno (*stat)(struct vfs_node *node, struct vfs_stat *out); // get file metadata

    enum errno (*readdir)(struct vfs_node *node, struct vfs_dirent *out,
                          uint64_t index); // read directory entry at index

    enum errno (*mkdir)(struct vfs_node *parent, const char *name, int mode); // create directory

    enum errno (*rmdir)(struct vfs_node *parent, const char *name); // remove directory

    enum errno (*unlink)(struct vfs_node *parent, const char *name); // delete file

    enum errno (*rename)(struct vfs_node *old_parent, const char *old_name,
                         struct vfs_node *new_parent, const char *new_name); // rename/move file or directory

    enum errno (*truncate)(struct vfs_node *node, uint64_t length); // resize file to given length

    enum errno (*readlink)(struct vfs_node *node, char *buf, uint64_t size); // read symlink target into buffer

    enum errno (*link)(struct vfs_node *parent, struct vfs_node *target,
                       const char *link_name); // create hard link to target

    enum errno (*chmod)(struct vfs_node *node, int mode); // change file permissions

    enum errno (*chown)(struct vfs_node *node, int uid, int gid); // change file ownership

    enum errno (*utime)(struct vfs_node *node, uint64_t atime,
                        uint64_t mtime); // update access and modification times

    void (*destroy)(struct vfs_node *node); 

    // enum errno (*ioctl)(struct vfs_node *node, uint64_t request, void *arg); // device-specific control (optional)

    struct vfs_node *(*finddir)(struct vfs_node *node,
                                const char *name); // find a child node by name
};

struct vfs_mount {
    struct vfs_node *root;
    struct generic_disk *disk;
    void *fs;
    char mount_point[256];
};

struct vfs_node {
    bool open;
    char name[256];
    uint32_t flags;
    enum vfs_node_type type;
    uint64_t size;

    void *fs_data;
    void *fs_node_data;

    struct vfs_ops *ops;
};
