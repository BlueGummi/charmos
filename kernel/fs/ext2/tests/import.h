#pragma once
#include <stdint.h>
#include <test/export.h>

TEST_IMPORT(uint32_t, ext2_to_vfs_flags, uint32_t ext2_flags);
TEST_IMPORT(uint32_t, vfs_to_ext2_flags, uint32_t vfs_flags);
TEST_IMPORT(uint16_t, ext2_to_vfs_mode, uint16_t ext2_mode);
TEST_IMPORT(uint16_t, vfs_to_ext2_mode, uint16_t vfs_mode);
