#include "sync/rwlock_internal.h"
#include <sync/lock_chk.h>
#include <sync/mutex.h>
#include <sync/qspinlock.h>
#include <sync/raw_spinlock.h>
#include <sync/rwlock.h>
#include <sync/spinlock.h>
#include <test/static_assert.h>

/* verification of lock word sizes and bitfield not overlapping */
static_assert_size(struct raw_spinlock, 1);
#ifndef DEBUG_LOCK_CHK
static_assert_size(struct qspinlock, 4);
static_assert_size(struct spinlock, 1);
static_assert_size(struct mutex, 8);
static_assert_size(struct rwlock, 8);
#endif

static_assert(LOCK_UNCHKD == 0);
static_assert(LOCK_CHKD_FULL == (LOCK_CHKD_ORDER | LOCK_CHKD_THREAD));

static_assert_disjoint_masks(Q_SPIN_LOCKED_MASK, Q_SPIN_PENDING_MASK);
static_assert_disjoint_masks(Q_SPIN_TAIL_MASK, Q_SPIN_LOCKED_PENDING_MASK);

static_assert_disjoint_masks(RWLOCK_OWNER_MASK,
                             (uintptr_t) (RWLOCK_WRITER_HELD_BIT |
                                          RWLOCK_PRIO_CEIL_MASK |
                                          RWLOCK_WAITER_BIT |
                                          RWLOCK_WRITER_WANT_BIT));
