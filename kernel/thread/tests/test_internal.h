#pragma once
#include <test/test.h>

#include <mem/alloc_or_die.h>
#include <sch/sched.h>
#include <string.h>
#include <thread/apc.h>
#include <thread/daemon.h>
#include <thread/reaper.h>
#include <thread/thread.h>
#include <thread/workqueue.h>
#include <time/spin_sleep.h>

TEST_GROUP_DEFINE(apc);
TEST_GROUP_DEFINE(daemon);
