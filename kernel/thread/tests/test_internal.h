#pragma once
#include <test.h>

#include <mem/alloc_or_die.h>
#include <sch/sched.h>
#include <string.h>
#include <thread/apc.h>
#include <thread/daemon.h>
#include <thread/reaper.h>
#include <thread/thread.h>
#include <thread/workqueue.h>
#include <time/spin_sleep.h>

extern struct test_group __test_group_apc;
extern struct test_group __test_group_daemon;
