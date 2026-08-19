#pragma once
#include <test/test.h>

#include <crypto/prng.h>
#include <errno.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <mem/anon_vma.h>
#include <mem/elcm.h>
#include <mem/folio.h>
#include <mem/mm.h>
#include <mem/page.h>
#include <mem/page_alloc.h>
#include <mem/page_table.h>
#include <mem/pmm.h>
#include <mem/rmap.h>
#include <mem/slab.h>
#include <mem/tlb.h>
#include <mem/vma_range.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <stack_depot.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <structures/rbit.h>
#include <thread/thread.h>

TEST_GROUP_DEFINE(mem);
TEST_GROUP_DEFINE(folio);
TEST_GROUP_DEFINE(mm);
TEST_GROUP_DEFINE(rmap);
TEST_GROUP_DEFINE(page_table);
TEST_GROUP_DEFINE(elcm);
