#include <stddef.h>
#include <stdint.h>

enum devtmpfs_node_type {
    DEVTMPFS_DIR,
    DEVTMPFS_FILE,
    DEVTMPFS_SYMLINK,
};

struct devtmpfs_node {
    enum devtmpfs_node_type type;
    struct vfs_node *vfs; // back-pointer to the VFS node

    struct devtmpfs_node **children; // of `devtmpfs_node*`

    char *data;
    uint64_t size;

    enum errno (*read)(struct vfs_node *, void *buf, uint64_t size,
                       uint64_t offset, void *ctx);
    enum errno (*write)(struct vfs_node *, const void *buf, uint64_t size,
                        uint64_t offset, void *ctx);
    void *rw_ctx;
};
