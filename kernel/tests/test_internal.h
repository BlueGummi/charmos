#pragma once
#include <test.h>

#include <crypto/prng.h>
#include <log.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stack_depot.h>
#include <stdatomic.h>
#include <string.h>
#include <thread/thread.h>

extern struct test_group __test_group_log;
extern struct test_group __test_group_stack_depot;
