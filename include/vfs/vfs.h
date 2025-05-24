#include <spin_lock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t off_t;

struct vfs_node;
struct vnode_ops;

enum vnode_type {
    VFS_FILE = 0x1,
    VFS_DIRECTORY = 0x2,
};

struct vfs_node {
    char *name;
    enum vnode_type type;
    uint32_t permissions;
    uint32_t uid, gid;
    uint64_t size;
    uint64_t inode;

    uint64_t atime, mtime, ctime;
    struct vnode_ops *ops;
    void *internal_data;

    uint32_t ref_count;
    bool is_mountpoint;

    struct spinlock lock;
};

uint64_t vfs_read(struct vfs_node *node, void *buf, size_t size, size_t offset);
void vfs_init();
void read_test();
#pragma once
