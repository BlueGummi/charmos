/* @title: Reader writer lock */
#pragma once
#include <compiler.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/lock_chk_types.h>
#include <thread/thread_types.h>

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
 * w - writer bit   -> a writer holds the lock
 * a - waiter bit   -> threads are waiting on the lock
 * W - writer want  -> a writer wants the lock
 * R - reader count -> used to store the number of readers
 * p - prio. ceil.  -> boosts threads to this ceiling
 *
 * %%%% - pointer to owner thread
 *
 */
struct rwlock {
    _Atomic(uintptr_t) lock_word;

#ifdef DEBUG_LOCK_CHK
    enum lock_chk_flags chk_flags;
    bool chk_initialized;
    _Atomic bool chk_used;
    struct lock_chk_map chk_map;
#endif /* DEBUG_LOCK_CHK */
};

enum rwlock_acquire_type {
    RWLOCK_ACQUIRE_READ = 0,
    RWLOCK_ACQUIRE_WRITE = 1,
};

void rw_lock_internal(struct rwlock *lock, enum rwlock_acquire_type type,
                      unsigned int subclass, const struct lock_chk_site *site);
void rw_unlock_internal(struct rwlock *lock, const struct lock_chk_site *site);
void rwlock_init_chk_internal(struct rwlock *lock,
                              enum thread_prio_class ceiling,
                              const struct lock_chk_class *class,
                              enum lock_chk_flags flags);
void rwlock_set_chk_flags(struct rwlock *lock, enum lock_chk_flags flags);
void rwlock_reinit_chk(struct rwlock *lock, enum thread_prio_class ceiling,
                       const struct lock_chk_class *class,
                       enum lock_chk_flags flags);
bool rwlock_locked(struct rwlock *lock, enum rwlock_acquire_type type);
void rwlock_assert_held_internal(struct rwlock *lock,
                                 enum rwlock_acquire_type type,
                                 const struct lock_chk_site *site);
void rwlock_assert_not_held_internal(struct rwlock *lock,
                                     const struct lock_chk_site *site);

#define RWLOCK_ASSERT_HELD(lock, type)                                         \
    rwlock_assert_held_internal((lock), (type), LOCK_CHK_SITE_HERE())
#define RWLOCK_ASSERT_READ(lock) RWLOCK_ASSERT_HELD((lock), RWLOCK_ACQUIRE_READ)
#define RWLOCK_ASSERT_WRITE(lock)                                              \
    RWLOCK_ASSERT_HELD((lock), RWLOCK_ACQUIRE_WRITE)
#define RWLOCK_ASSERT_NOT_HELD(lock)                                           \
    rwlock_assert_not_held_internal((lock), LOCK_CHK_SITE_HERE())

#define RWLOCK_PRIO_CEIL_SHIFT (1)

#ifdef DEBUG_LOCK_CHK

#define RWLOCK_INIT_CHK(ceil_, class_, flags_)                                 \
    ((struct rwlock) {                                                         \
        .lock_word = ATOMIC_VAR_INIT((ceil_) << RWLOCK_PRIO_CEIL_SHIFT),       \
        .chk_flags = (flags_),                                                 \
        .chk_initialized = true,                                               \
        .chk_used = ATOMIC_VAR_INIT(false),                                    \
        .chk_map = LOCK_CHK_MAP_VALUE_INIT(class_),                            \
    })

#define rwlock_init_chk(lock_, ceil_, class_, flags_)                          \
    rwlock_init_chk_internal((lock_), (ceil_), (class_), (flags_))
#define rwlock_init_auto_internal(lock_, ceil_, flags_)                        \
    do {                                                                       \
        static const struct lock_chk_class __auto_class = {                    \
            .name = #lock_,                                                    \
            .file = __RELFILE__,                                               \
            .line = __LINE__,                                                  \
        };                                                                     \
        rwlock_init_chk_internal((lock_), (ceil_), &__auto_class, (flags_));   \
    } while (0)

#else /* !defined(DEBUG_LOCK_CHK) */

#define RWLOCK_INIT_CHK(ceil_, class_, flags_)                                 \
    ((struct rwlock) {                                                         \
        .lock_word = ATOMIC_VAR_INIT((ceil_) << RWLOCK_PRIO_CEIL_SHIFT),       \
    })

#define rwlock_init_chk(lock_, ceil_, class_, flags_)                          \
    rwlock_init_chk_internal((lock_), (ceil_), NULL, LOCK_UNCHKD)
#define rwlock_init_auto_internal(lock_, ceil_, flags_)                        \
    rwlock_init_chk_internal((lock_), (ceil_), NULL, LOCK_UNCHKD)

#endif /* DEBUG_LOCK_CHK */

#define RWLOCK_INIT(ceil_) RWLOCK_INIT_CHK((ceil_), NULL, LOCK_CHKD_FULL)
#define RWLOCK_DEFINE(id, ceil_) struct rwlock id = RWLOCK_INIT(ceil_)
#define RWLOCK_DEFINE_CHK(id, ceil_, class_, flags_)                           \
    struct rwlock id = RWLOCK_INIT_CHK((ceil_), (class_), (flags_))

#define rwlock_init_2(lock_, ceil_)                                            \
    rwlock_init_auto_internal((lock_), (ceil_), LOCK_CHKD_FULL)
#define rwlock_init_3(lock_, ceil_, flags_)                                    \
    rwlock_init_auto_internal((lock_), (ceil_), (flags_))
#define rwlock_init(...)                                                       \
    _DISPATCH(rwlock_init, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define rw_lock(lock_, type_)                                                  \
    rw_lock_internal((lock_), (type_), 0, LOCK_CHK_SITE_HERE())
#define rw_read_lock(lock_)                                                    \
    rw_lock_internal((lock_), RWLOCK_ACQUIRE_READ, 0, LOCK_CHK_SITE_HERE())
#define rw_write_lock(lock_)                                                   \
    rw_lock_internal((lock_), RWLOCK_ACQUIRE_WRITE, 0, LOCK_CHK_SITE_HERE())
#define rw_read_lock_subclass(lock_, subclass_)                                \
    rw_lock_internal((lock_), RWLOCK_ACQUIRE_READ, (subclass_),                \
                     LOCK_CHK_SITE_HERE())
#define rw_write_lock_subclass(lock_, subclass_)                               \
    rw_lock_internal((lock_), RWLOCK_ACQUIRE_WRITE, (subclass_),               \
                     LOCK_CHK_SITE_HERE())
#define rw_unlock(lock_) rw_unlock_internal((lock_), LOCK_CHK_SITE_HERE())
