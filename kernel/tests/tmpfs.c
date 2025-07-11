#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <tests.h>

#include "errno.h"
#include "string.h"

#define TMPFS_SETUP_NODE(root, node, name, e)                                  \
    struct vfs_node *root = tmpfs_mkroot("tmp");                               \
    TEST_ASSERT(root != NULL);                                                 \
    enum errno e = root->ops->create(root, name, VFS_MODE_FILE);               \
    struct vfs_dirent ent;                                                     \
    struct vfs_node *node;                                                     \
    e = root->ops->finddir(root, name, &ent);                                  \
    TEST_ASSERT(!ERR_IS_FATAL(e));                                             \
    node = ent.node;                                                           \
    TEST_ASSERT(node != NULL);

REGISTER_TEST(tmpfs_rw_test, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    TMPFS_SETUP_NODE(root, node, "place", e);
    TEST_ASSERT(node->size == 0);

    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);

    e = node->ops->write(node, lstr, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));
    TEST_ASSERT(node->size == len);

    char *out_buf = kzalloc(len);
    TEST_ASSERT(out_buf != NULL);
    e = node->ops->read(node, out_buf, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    TEST_ASSERT(memcmp(out_buf, lstr, len) == 0);

    e = node->ops->truncate(node, len / 2);
    TEST_ASSERT(!ERR_IS_FATAL(e));
    TEST_ASSERT(node->size == len / 2);

    memset(out_buf, 0, len);
    e = node->ops->read(node, out_buf, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    e = node->ops->unlink(root, "place");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    e = node->ops->destroy(node);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    e = root->ops->finddir(root, "place", &ent);
    TEST_ASSERT(e == ERR_NO_ENT);

    TEST_ASSERT(strlen(out_buf) == len / 2);
    SET_SUCCESS;
}

REGISTER_TEST(tmpfs_dir_test, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    struct vfs_node *root = tmpfs_mkroot("tmp");
    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);

    char *out_buf = kzalloc(len);
    TEST_ASSERT(out_buf != NULL);

    enum errno e = root->ops->mkdir(root, "place", VFS_MODE_DIR);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_dirent ent;
    struct vfs_node *dir;
    e = root->ops->finddir(root, "place", &ent);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    dir = ent.node;
    TEST_ASSERT(dir != NULL);

    e = dir->ops->write(dir, lstr, len, 0);
    TEST_ASSERT(e == ERR_IS_DIR);

    e = dir->ops->read(dir, out_buf, len, 0);
    TEST_ASSERT(e == ERR_IS_DIR);

    e = dir->ops->rmdir(root, "place");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    e = root->ops->finddir(root, "place", &ent);
    TEST_ASSERT(e == ERR_NO_ENT);

    SET_SUCCESS;
}

REGISTER_TEST(tmpfs_general_tests, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    TMPFS_SETUP_NODE(root, node, "place", e);

    e = node->ops->chmod(node, VFS_MODE_EXEC);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    TEST_ASSERT(node->mode == VFS_MODE_EXEC);

    e = node->ops->chown(node, 42, 37);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    TEST_ASSERT(node->uid == 42 && node->gid == 37);

    e = root->ops->mkdir(root, "bingbong", VFS_MODE_DIR);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    e = root->ops->finddir(root, "bingbong", &ent);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    node = ent.node;
    TEST_ASSERT(node != NULL);

    e = node->ops->symlink(node, "/tmp", "bang");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_node *bang;
    e = node->ops->finddir(node, "bang", &ent);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    bang = ent.node;
    TEST_ASSERT(bang != NULL);

    char *buf = kzalloc(10);
    TEST_ASSERT(bang != NULL);

    e = bang->ops->readlink(bang, buf, 10);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    TEST_ASSERT(strcmp(buf, "/tmp") == 0);

    SET_SUCCESS;
}
