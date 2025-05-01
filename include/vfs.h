#include <stdint.h>
#include <stddef.h>

#define VFS_FILE        0x1
#define VFS_DIRECTORY   0x2

struct vfs_node;

typedef uint64_t (*read_fn_t)(struct vfs_node*, void*, size_t, size_t);
typedef uint64_t (*write_fn_t)(struct vfs_node*, const void*, size_t, size_t);

struct vfs_node {
    char name[64];
    uint32_t flags;
    size_t size;
    
    read_fn_t read;
    write_fn_t write;
    
    struct vfs_node* parent;
    struct vfs_node* children;
    struct vfs_node* next_sibling;
    
    void* data;
};

uint64_t memfs_read(struct vfs_node* node, void* buf, size_t size, size_t offset);
void vfs_init();
void read_test();
#pragma once

