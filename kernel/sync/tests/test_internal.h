#pragma once
#include <test.h>

#include <crypto/prng.h>
#include <log.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <string.h>
#include <sync/mutex.h>
#include <sync/rcu.h>
#include <sync/rwlock.h>
#include <thread/apc.h>
#include <thread/thread.h>
#include <thread/workqueue.h>
#include <time/spin_sleep.h>

extern struct test_group __test_group_mutex;
extern struct test_group __test_group_rcu;
extern struct test_group __test_group_rwlock;
extern struct test_group __test_group_sync_nightmare;
