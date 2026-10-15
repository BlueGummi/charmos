#pragma once
#include <test/test.h>

#include <crypto/prng.h>
#include <math/fixed.h>
#include <math/min_max.h>
#include <mem/alloc.h>
#include <structures/bitmap.h>
#include <structures/minheap.h>
#include <structures/rbit.h>
#include <structures/rbt.h>

#include <structures/avl.h>
#include <structures/bloom.h>
#include <structures/radix.h>
#include <structures/splay.h>
#include <structures/treap.h>

#include <structures/cpu_mask.h>
#include <structures/id_space.h>
#include <structures/list.h>
#include <structures/mpmc_fifo_ring.h>
#include <structures/spsc_fifo_ring.h>

TEST_GROUP_EXTERN(list);
TEST_GROUP_EXTERN(minheap);
TEST_GROUP_EXTERN(rbt);
TEST_GROUP_EXTERN(rbit);
TEST_GROUP_EXTERN(bitmap);
TEST_GROUP_EXTERN(radix);
TEST_GROUP_EXTERN(avl);
TEST_GROUP_EXTERN(bloom);
TEST_GROUP_EXTERN(splay);
TEST_GROUP_EXTERN(treap);
TEST_GROUP_EXTERN(mpmc_fifo_ring);
TEST_GROUP_EXTERN(spsc_fifo_ring);
TEST_GROUP_EXTERN(id_space);
TEST_GROUP_EXTERN(cpu_mask);
