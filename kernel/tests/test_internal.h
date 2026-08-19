#pragma once
#include <test/test.h>

#include <crypto/prng.h>
#include <log.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stack_depot.h>
#include <stdatomic.h>
#include <string.h>
#include <thread/thread.h>

#include <parse.h>

TEST_GROUP_DEFINE(log);
TEST_GROUP_DEFINE(stack_depot);
TEST_GROUP_DEFINE(parse);
TEST_GROUP_DEFINE(string);
