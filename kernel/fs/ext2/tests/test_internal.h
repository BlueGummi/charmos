#pragma once
#include <test/test.h>

#include <block/sched.h>
#include <fs/ext2.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <string.h>
#include <time/spin_sleep.h>

#include "import.h"

TEST_GROUP_DEFINE(ext2);
TEST_GROUP_DEFINE(ext2_mode);
