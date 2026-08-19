#include "../../rwlock_internal.h"
#include <sync/qspinlock.h>
#include <test/static_assert.h>

/* verification of lock word sizes and bitfield not overlapping */
static_assert_size(struct qspinlock, 4);

static_assert_disjoint_masks(Q_SPIN_LOCKED_MASK, Q_SPIN_PENDING_MASK);
static_assert_disjoint_masks(Q_SPIN_TAIL_MASK, Q_SPIN_LOCKED_PENDING_MASK);

static_assert_disjoint_masks(RWLOCK_OWNER_MASK,
                             (uintptr_t) (RWLOCK_WRITER_HELD_BIT |
                                          RWLOCK_PRIO_CEIL_MASK |
                                          RWLOCK_WAITER_BIT |
                                          RWLOCK_WRITER_WANT_BIT));
