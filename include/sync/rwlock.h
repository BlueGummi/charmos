/* @title: Reader writer lock */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <math/bit.h>
#include <sch/irql.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/lock_chk_types.h>
#include <sync/lock_general.h>
#include <thread/thread_types.h>
#include <tsa.h>

typedef BITMAP_DECLARE(rwlock_mask_t, THREAD_RW_READER_ID_BITS);
typedef uint8_t rwlock_id_t;
static_assert(BITS_PER_BYTE * sizeof(rwlock_id_t) >=
                  BITS_NEEDED(THREAD_RW_READER_ID_BITS),
              "rwlock_id_t needs to be wider");
/*
 * Our rwlocks perform a variant of priority ceiling protocols lazily through
 * the use of a strategy similar to a bloom filter.
 */

/* rwlock: pointer sized shared reader writer lock
 *
 * note: the writer bit leads to two separate
 * possible encodings for the rest of the bits in the lock word.
 *
 *                ┌─────────────────────────┐
 * Bits           │  ....  ....  ....  3..0 │
 * Use when w = 1 │  %%%%  %%%%  %%%%  %ppw │
 * Use when w = 0 │  RRRR  RRRR  RRRW  appw │
 *                └─────────────────────────┘
 *
 *
 * w - writer bit     -> a writer holds the lock
 * a - waiter bit     -> threads are waiting on the lock
 * W - writer want    -> a writer wants the lock
 * R - reader count   -> used to store the number of readers
 * m - max prio class -> panic if reader > this
 *
 * %%%% - pointer to owner thread
 *
 */
struct TSA_CAPABILITY("rwlock") rwlock {
    atomic_uintptr_t lock_word;

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_lock chk;
#endif /* DEBUG_LOCK_CHK */
};

enum rwlock_acquire_type {
    RWLOCK_READ = 0,
    RWLOCK_WRITE = 1,
};

void rw_lock_full(struct rwlock *lock, enum rwlock_acquire_type type,
                  enum lock_subclass subclass, const struct lock_chk_site *site)
    TSA_ACQUIRES(lock) TSA_MAY_SLEEP;
void rw_unlock_full(struct rwlock *lock, const struct lock_chk_site *site)
    TSA_RELEASES(lock);
void rwlock_init_chk_full(struct rwlock *lock, enum thread_prio_class ceiling,
                          const struct lock_chk_class *class,
                          enum lock_chk_flags flags);
void rwlock_set_chk_flags(struct rwlock *lock, enum lock_chk_flags flags);
void rwlock_reinit_chk(struct rwlock *lock, enum thread_prio_class ceiling,
                       const struct lock_chk_class *class,
                       enum lock_chk_flags flags);
bool rwlock_locked(struct rwlock *lock, enum rwlock_acquire_type type);
void rwlock_assert_held_full(struct rwlock *lock, enum rwlock_acquire_type type,
                             const struct lock_chk_site *site)
    TSA_ASSERT_CAPABILITY(lock);
void rwlock_assert_not_held_full(struct rwlock *lock,
                                 const struct lock_chk_site *site);

#define RWLOCK_ASSERT_HELD(lock, type)                                         \
    rwlock_assert_held_full((lock), (type), LOCK_CHK_SITE_HERE())
#define RWLOCK_ASSERT_READ(lock) RWLOCK_ASSERT_HELD((lock), RWLOCK_READ)
#define RWLOCK_ASSERT_WRITE(lock) RWLOCK_ASSERT_HELD((lock), RWLOCK_WRITE)
#define RWLOCK_ASSERT_NOT_HELD(lock)                                           \
    rwlock_assert_not_held_full((lock), LOCK_CHK_SITE_HERE())

#define RWLOCK_PRIO_CEIL_SHIFT 1

#ifdef DEBUG_LOCK_CHK

#define RWLOCK_INIT_CHK(ceil_, class_, flags_)                                 \
    ((struct rwlock) {                                                         \
        .lock_word = ATOMIC_VAR_INIT((ceil_) << RWLOCK_PRIO_CEIL_SHIFT),       \
        .chk = LOCK_CHK_LOCK_VALUE_INIT((class_), (flags_)),                   \
    })

#define rwlock_init_chk(lock_, ceil_, class_, flags_)                          \
    rwlock_init_chk_full((lock_), (ceil_), (class_), (flags_))
#define rwlock_init_auto_internal(lock_, ceil_, flags_)                        \
    do {                                                                       \
        static const struct lock_chk_class __auto_class = {                    \
            .name = #lock_,                                                    \
            .file = __RELFILE__,                                               \
            .line = __LINE__,                                                  \
        };                                                                     \
        rwlock_init_chk_full((lock_), (ceil_), &__auto_class, (flags_));       \
    } while (0)

#else /* !defined(DEBUG_LOCK_CHK) */

#define RWLOCK_INIT_CHK(ceil_, class_, flags_)                                 \
    ((struct rwlock) {                                                         \
        .lock_word = ATOMIC_VAR_INIT((ceil_) << RWLOCK_PRIO_CEIL_SHIFT),       \
    })

#define rwlock_init_chk(lock_, ceil_, class_, flags_)                          \
    rwlock_init_chk_full((lock_), (ceil_), NULL, LOCK_UNCHKD)
#define rwlock_init_auto_internal(lock_, ceil_, flags_)                        \
    rwlock_init_chk_full((lock_), (ceil_), NULL, LOCK_UNCHKD)

#endif /* DEBUG_LOCK_CHK */

#define RWLOCK_INIT(ceil_) RWLOCK_INIT_CHK((ceil_), NULL, LOCK_CHKD_FULL)
#define RWLOCK_DEFINE(id, ceil_) struct rwlock id = RWLOCK_INIT(ceil_)
#define RWLOCK_DEFINE_CHK(id, ceil_, class_, flags_)                           \
    struct rwlock id = RWLOCK_INIT_CHK((ceil_), (class_), (flags_))

#define rwlock_init_2(lock_, ceil_) rwlock_init_3(lock_, ceil_, LOCK_CHKD_FULL)
#define rwlock_init_3(lock_, ceil_, flags_)                                    \
    rwlock_init_auto_internal((lock_), (ceil_), (flags_))
#define rwlock_init(...) PP_CALL(rwlock_init, __VA_ARGS__)

#define rw_lock(lock_, type_)                                                  \
    rw_lock_full((lock_), (type_), LOCK_SUBCLASS_PRIMARY, LOCK_CHK_SITE_HERE())
#define rw_read_lock(lock_)                                                    \
    rw_lock_full((lock_), RWLOCK_READ, LOCK_SUBCLASS_PRIMARY,                  \
                 LOCK_CHK_SITE_HERE())
#define rw_write_lock(lock_)                                                   \
    rw_lock_full((lock_), RWLOCK_WRITE, LOCK_SUBCLASS_PRIMARY,                 \
                 LOCK_CHK_SITE_HERE())
#define rw_read_lock_subclass(lock_, subclass_)                                \
    rw_lock_full((lock_), RWLOCK_READ, (subclass_), LOCK_CHK_SITE_HERE())
#define rw_write_lock_subclass(lock_, subclass_)                               \
    rw_lock_full((lock_), RWLOCK_WRITE, (subclass_), LOCK_CHK_SITE_HERE())
#define rw_unlock(lock_) rw_unlock_full((lock_), LOCK_CHK_SITE_HERE())

static inline cc_always_inline
cc_unused void rwlock_guard_exit(struct rwlock **lock) TSA_NO_ANALYSIS {
    if (*lock)
        rw_unlock(*lock);
}

static inline cc_always_inline cc_unused void
rwlock_guard_assume(struct rwlock *lock) TSA_ASSERT_CAPABILITY(lock) {
    cc_ignore(lock);
}

static inline cc_always_inline cc_unused struct rwlock *
rw_read_guard_enter(struct rwlock *lock) TSA_MAY_SLEEP TSA_NO_ANALYSIS {
    rw_read_lock(lock);
    return lock;
}

static inline cc_always_inline cc_unused struct rwlock *
rw_write_guard_enter(struct rwlock *lock) TSA_MAY_SLEEP TSA_NO_ANALYSIS {
    rw_write_lock(lock);
    return lock;
}

#define rw_read_guard(lock_)                                                   \
    cc_cleanup(rwlock_guard_exit) struct rwlock *PP_CONCAT(rw_read_guard_,     \
                                                           __COUNTER__) =      \
        (rwlock_guard_assume(lock_), rw_read_guard_enter(lock_))

#define rw_write_guard(lock_)                                                  \
    cc_cleanup(rwlock_guard_exit) struct rwlock *PP_CONCAT(rw_write_guard_,    \
                                                           __COUNTER__) =      \
        (rwlock_guard_assume(lock_), rw_write_guard_enter(lock_))
