#pragma once
#include <test/test.h>

#include "../internal.h"
#include "import.h"
#include <mem/alloc_or_die.h>
#include <sch/sched.h>
#include <string.h>
#include <thread/apc.h>
#include <thread/daemon.h>
#include <thread/reaper.h>
#include <thread/thread.h>
#include <thread/workqueue.h>

TEST_GROUP_DEFINE(sched);
TEST_GROUP_DEFINE(climb);
