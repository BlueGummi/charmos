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
#include <structures/splay.h>
#include <structures/treap.h>

#include <structures/cpu_mask.h>
#include <structures/id_space.h>
#include <structures/mpmc_queue.h>
#include <structures/spsc_fifo.h>

TEST_GROUP_DEFINE(minheap);
TEST_GROUP_DEFINE(rbit);
TEST_GROUP_DEFINE(bitmap);
TEST_GROUP_DEFINE(radix);
TEST_GROUP_DEFINE(avl);
TEST_GROUP_DEFINE(bloom);
TEST_GROUP_DEFINE(splay);
TEST_GROUP_DEFINE(treap);
TEST_GROUP_DEFINE(mpmc_queue);
TEST_GROUP_DEFINE(spsc_fifo);
TEST_GROUP_DEFINE(id_space);
TEST_GROUP_DEFINE(cpu_mask);
