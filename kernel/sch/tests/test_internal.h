#pragma once
#include <test/test.h>

#include "import.h"
#include "sch/internal.h"
#include <mem/nofail.h>
#include <sch/sched.h>
#include <string.h>
#include <thread/apc.h>
#include <thread/daemon.h>
#include <thread/reaper.h>
#include <thread/thread.h>
#include <thread/workqueue.h>

TEST_GROUP_EXTERN(sched);
TEST_GROUP_EXTERN(climb);
