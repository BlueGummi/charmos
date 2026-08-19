#pragma once
#include <test/test.h>

#include <crypto/prng.h>
#include <math/fixed.h>
#include <mem/alloc.h>
#include <structures/bitmap.h>
#include <structures/minheap.h>
#include <structures/rbit.h>

#include <structures/avl.h>
#include <structures/bloom.h>
#include <structures/radix.h>

TEST_GROUP_DEFINE(minheap);
TEST_GROUP_DEFINE(rbit);
TEST_GROUP_DEFINE(bitmap);
TEST_GROUP_DEFINE(radix);
TEST_GROUP_DEFINE(avl);
TEST_GROUP_DEFINE(bloom);
