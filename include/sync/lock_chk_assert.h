/* @title: Lock Validator Assertions */
#pragma once
#include <compiler.h>
#include <sync/mutex.h>
#include <sync/mutex_simple.h>
#include <sync/qspinlock.h>
#include <sync/rwlock.h>
#include <sync/spinlock.h>

/* Assert that the current thread holds the lock. When DEBUG_LOCK_CHK
 * is on and the lock is checked, this actually gets to use
 * the provable engine. But otherwise, it falls back to a naive
 * read of the lock word. RWLOCK needs
 * RWLOCK_ACQUIRE_READ/RWLOCK_ACQUIRE_WRITE, which is why we have
 * _1 and _2, so all locks regardless of type can funnel through
 * this callsite */

#define LOCK_CHK_ASSERT_HELD_1(l)                                              \
    _Generic((l),                                                              \
        struct spinlock *: spinlock_assert_held_internal,                      \
        struct qspinlock *: qspin_assert_held_internal,                        \
        struct mutex *: mutex_assert_held_internal,                            \
        struct mutex_simple *: mutex_simple_assert_held_internal)(             \
        (l), LOCK_CHK_SITE_HERE())

#define LOCK_CHK_ASSERT_HELD_2(l, mode)                                        \
    _Generic((l), struct rwlock *: rwlock_assert_held_internal)(               \
        (l), (mode), LOCK_CHK_SITE_HERE())

#define LOCK_CHK_ASSERT_HELD(...)                                              \
    _DISPATCH(LOCK_CHK_ASSERT_HELD, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

/* Inverse of above */
#define LOCK_CHK_ASSERT_NOT_HELD(l)                                            \
    _Generic((l),                                                              \
        struct spinlock *: spinlock_assert_not_held_internal,                  \
        struct qspinlock *: qspin_assert_not_held_internal,                    \
        struct mutex *: mutex_assert_not_held_internal,                        \
        struct mutex_simple *: mutex_simple_assert_not_held_internal,          \
        struct rwlock *: rwlock_assert_not_held_internal)(                     \
        (l), LOCK_CHK_SITE_HERE())
