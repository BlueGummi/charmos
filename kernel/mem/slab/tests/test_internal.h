#pragma once
#include <test/test.h>

#include "mem/slab/internal.h"
#include <crypto/prng.h>
#include <mem/alloc.h>
#include <mem/elcm.h>
#include <mem/page_alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/tlb.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <stack_depot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <thread/thread.h>

TEST_GROUP_DEFINE(slab);
extern struct test_group __test_group_slab;
