#pragma once
#include <test.h>

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

extern struct test_group __test_group_mem;
extern struct test_group __test_group_folio;
extern struct test_group __test_group_mm;
extern struct test_group __test_group_rmap;
