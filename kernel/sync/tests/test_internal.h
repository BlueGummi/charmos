#pragma once
#include <test/test.h>

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

#include <sync/qspinlock.h>
#include <sync/turnstile.h>

TEST_GROUP_DEFINE(mutex);
TEST_GROUP_DEFINE(rcu);
TEST_GROUP_DEFINE(rwlock);
TEST_GROUP_DEFINE(sync_nightmare);
TEST_GROUP_DEFINE(qspinlock);
TEST_GROUP_DEFINE(turnstile);
