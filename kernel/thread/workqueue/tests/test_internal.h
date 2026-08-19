#pragma once
#include <test/test.h>

#include "../internal.h"
#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <thread/apc.h>
#include <thread/daemon.h>
#include <thread/reaper.h>
#include <thread/thread.h>
#include <thread/workqueue.h>
#include <time/spin_sleep.h>
#include <time/time.h>

TEST_GROUP_DEFINE(workqueue);
TEST_GROUP_DEFINE(defer);
