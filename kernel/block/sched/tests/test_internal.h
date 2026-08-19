#pragma once
#include <test/test.h>

#include "fs/detect.h"
#include <block/block.h>
#include <block/sched.h>
#include <crypto/prng.h>
#include <fs/ext2.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time/spin_sleep.h>

TEST_GROUP_DEFINE(bio_sched);
