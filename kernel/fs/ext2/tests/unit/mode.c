#include "../test_internal.h"

#ifdef TEST_EXT2
TEST_GROUP_DECLARE(ext2_mode);

/* ext2 file type is a 4 bit enumerated field in the top nibble of mode */
struct ftype_case {
    uint16_t mode;
    uint8_t expect;
    const char *name;
};

static const struct ftype_case ftype_cases[] = {
    {EXT2_S_IFREG, EXT2_FT_REG_FILE, "regular"},
    {EXT2_S_IFDIR, EXT2_FT_DIR, "directory"},
    {EXT2_S_IFCHR, EXT2_FT_CHRDEV, "chardev"},
    {EXT2_S_IFBLK, EXT2_FT_BLKDEV, "blockdev"},
    {EXT2_S_IFIFO, EXT2_FT_FIFO, "fifo"},
    {EXT2_S_IFSOCK, EXT2_FT_SOCK, "socket"},
    {EXT2_S_IFLNK, EXT2_FT_SYMLINK, "symlink"},
};

TEST_DECLARE_UNIT(ext2_ftype_all_types, .group = TEST_GROUP(ext2_mode)) {
    for (size_t i = 0; i < TEST_ARRAY_LEN(ftype_cases); i++) {
        const struct ftype_case *c = &ftype_cases[i];
        uint8_t got = ext2_extract_ftype(c->mode);

        if (got != c->expect) {
            test_err("ftype(%s / %04x) = %u, want %u", c->name, c->mode, got,
                     c->expect);
            return TEST_FAIL("ext2_extract_ftype");
        }
    }

    return TEST_SUCCESS;
}

/* Perm bits share word with type */
TEST_DECLARE_UNIT(ext2_ftype_ignores_permissions,
                  .group = TEST_GROUP(ext2_mode)) {
    const uint16_t perms = 0x0FFF;

    for (size_t i = 0; i < TEST_ARRAY_LEN(ftype_cases); i++) {
        const struct ftype_case *c = &ftype_cases[i];
        TEST_ASSERT(ext2_extract_ftype(c->mode | perms) == c->expect);
        TEST_ASSERT(ext2_extract_ftype(c->mode) == c->expect);
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ext2_ftype_unknown, .group = TEST_GROUP(ext2_mode)) {
    /* Zero type field is not any of the seven, must not be guessed */
    TEST_ASSERT(ext2_extract_ftype(0) == EXT2_FT_UNKNOWN);
    TEST_ASSERT(ext2_extract_ftype(0x0FFF) == EXT2_FT_UNKNOWN);

    /* Each type maps to a distinct value in the defined range */
    for (size_t i = 0; i < TEST_ARRAY_LEN(ftype_cases); i++) {
        TEST_ASSERT(ftype_cases[i].expect < EXT2_FT_MAX);
        for (size_t j = i + 1; j < TEST_ARRAY_LEN(ftype_cases); j++)
            TEST_ASSERT(ftype_cases[i].expect != ftype_cases[j].expect);
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ext2_mode_type_roundtrip, .group = TEST_GROUP(ext2_mode)) {
    for (size_t i = 0; i < TEST_ARRAY_LEN(ftype_cases); i++) {
        uint16_t ext2 = ftype_cases[i].mode;
        uint16_t vfs = TEST_CALL(ext2_to_vfs_mode)(ext2);

        TEST_ASSERT((TEST_CALL(vfs_to_ext2_mode)(vfs) & EXT2_S_IFMT) ==
                    (ext2 & EXT2_S_IFMT));
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ext2_mode_permission_roundtrip,
                  .group = TEST_GROUP(ext2_mode)) {
    static const uint16_t perm_bits[] = {
        EXT2_S_IRUSR, EXT2_S_IWUSR, EXT2_S_IXUSR, EXT2_S_IRGRP, EXT2_S_IWGRP,
        EXT2_S_IXGRP, EXT2_S_IROTH, EXT2_S_IWOTH, EXT2_S_IXOTH,
    };

    /* One bit at a time isolates a swapped owner/group/other mappings, which
     * 0777 round trip would hide away */
    for (size_t i = 0; i < TEST_ARRAY_LEN(perm_bits); i++) {
        uint16_t ext2 = EXT2_S_IFREG | perm_bits[i];
        uint16_t back =
            TEST_CALL(vfs_to_ext2_mode)(TEST_CALL(ext2_to_vfs_mode)(ext2));

        TEST_ASSERT(back == ext2);
    }

    /* full set together */
    uint16_t all = EXT2_S_IFREG;
    for (size_t i = 0; i < TEST_ARRAY_LEN(perm_bits); i++)
        all |= perm_bits[i];

    TEST_ASSERT(TEST_CALL(vfs_to_ext2_mode)(TEST_CALL(ext2_to_vfs_mode)(all)) ==
                all);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ext2_flags_roundtrip, .group = TEST_GROUP(ext2_mode)) {
    static const uint32_t flags[] = {
        EXT2_APPEND_FL, EXT2_IMMUTABLE_FL, EXT2_NOATIME_FL,
        EXT2_SYNC_FL,   EXT2_DIRSYNC_FL,
    };

    uint32_t all = 0;
    for (size_t i = 0; i < TEST_ARRAY_LEN(flags); i++) {
        uint32_t back = TEST_CALL(vfs_to_ext2_flags)(
            TEST_CALL(ext2_to_vfs_flags)(flags[i]));

        TEST_ASSERT(back == flags[i]);
        all |= flags[i];
    }

    TEST_ASSERT(
        TEST_CALL(vfs_to_ext2_flags)(TEST_CALL(ext2_to_vfs_flags)(all)) == all);

    TEST_ASSERT(TEST_CALL(ext2_to_vfs_flags)(0) == 0);
    TEST_ASSERT(TEST_CALL(vfs_to_ext2_flags)(0) == 0);

    /* Flags we don't model mustn't be dropped */
    TEST_ASSERT(
        (TEST_CALL(vfs_to_ext2_flags)(TEST_CALL(ext2_to_vfs_flags)(~all)) &
         all) == 0);

    return TEST_SUCCESS;
}

#endif
