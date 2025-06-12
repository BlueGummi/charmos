#include <stdbool.h>
#include <stdint.h>
#pragma once

#define VFS_NAME_MAX 256

#define VFS_MODE_READ 0x0001
#define VFS_MODE_WRITE 0x0002
#define VFS_MODE_EXEC 0x0004
#define VFS_MODE_DIR 0x4000
#define VFS_MODE_FILE 0x8000
#define VFS_MODE_SYMLINK 0xA000

enum vfs_node_type {
    VFS_FILE,
    VFS_DIR,
    VFS_SYMLINK,
    VFS_CHARDEV,
    VFS_BLOCKDEV,
    VFS_PIPE,
    VFS_SOCKET
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
                     uint64_t offset);
    uint64_t (*write)(struct vfs_node *node, const void *buf, uint64_t size,
                      uint64_t offset);
    int (*open)(struct vfs_node *node, int flags);
    int (*close)(struct vfs_node *node);
    int (*stat)(struct vfs_node *node, struct vfs_stat *out);
    int (*readdir)(struct vfs_node *node, struct vfs_dirent *out,
                   uint64_t index);
    struct vfs_node *(*finddir)(struct vfs_node *node, const char *name);
};

struct vfs_node {
    char name[256];
    uint32_t flags;
    enum vfs_node_type type;
    uint64_t size;

    struct vfs_node *parent;
    struct vfs_mount *mount;

    void *fs_data;

    struct vfs_ops *ops;
};

struct vfs_mount {
    struct vfs_node *root;
    struct generic_disk *disk;
    struct vfs_filesystem *fs;
    char mount_point[256];
};
