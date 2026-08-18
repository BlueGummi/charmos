#pragma once
#include <test.h>

#include "fs/detect.h"
#include <block/bio.h>
#include <block/block.h>
#include <fs/ext2.h>
#include <fs/vfs.h>
#include <global.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time/spin_sleep.h>

extern struct test_group __test_group_bio;
