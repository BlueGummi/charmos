/* @title: Simple Mutex */
#pragma once
#include <stdbool.h>
#include <sync/lock_chk_types.h>
#include <sync/spinlock.h>
#include <thread/wait.h>
#include <tsa.h>

struct TSA_CAPABILITY("mutex") mutex_simple {
    struct thread *owner;
    struct thread_wait_header waiters;
    struct spinlock lock;

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_lock chk;
#endif /* DEBUG_LOCK_CHK */
};

void mutex_simple_init_chk_full(struct mutex_simple *m,
                                const struct lock_chk_class *class,
                                enum lock_chk_flags flags);
void mutex_simple_set_chk_flags(struct mutex_simple *m,
                                enum lock_chk_flags flags);
void mutex_simple_reinit_chk(struct mutex_simple *m,
                             const struct lock_chk_class *class,
                             enum lock_chk_flags flags);
void mutex_simple_lock_full(struct mutex_simple *m,
                            const struct lock_chk_site *site)
    TSA_ACQUIRES(m) TSA_MAY_SLEEP;

void mutex_simple_unlock_full(struct mutex_simple *m,
                              const struct lock_chk_site *site) TSA_RELEASES(m);

void mutex_simple_lock_subclass_full(struct mutex_simple *m,
                                     enum lock_subclass subclass,
                                     const struct lock_chk_site *site)
    TSA_ACQUIRES(m) TSA_MAY_SLEEP;

bool mutex_simple_locked(struct mutex_simple *m);
struct thread *mutex_simple_get_owner(struct mutex_simple *m);

void mutex_simple_assert_held_full(struct mutex_simple *m,
                                   const struct lock_chk_site *site)
    TSA_ASSERT_CAPABILITY(m);

void mutex_simple_assert_not_held_full(struct mutex_simple *m,
                                       const struct lock_chk_site *site);

#ifdef DEBUG_LOCK_CHK

#define MUTEX_SIMPLE_INIT_CHK(id_, class_, flags_)                             \
    ((struct mutex_simple) {                                                   \
        .owner = NULL,                                                         \
        .waiters =                                                             \
            {                                                                  \
                .waiters = LIST_HEAD_INIT((id_).waiters.waiters),              \
                .lock = SPINLOCK_INIT_CHK(NULL, LOCK_UNCHKD),                  \
            },                                                                 \
        .lock = SPINLOCK_INIT_CHK(NULL, LOCK_UNCHKD),                          \
        .chk = LOCK_CHK_LOCK_VALUE_INIT((class_), (flags_)),                   \
    })

#define mutex_simple_init_chk(mtx_, class_, flags_)                            \
    mutex_simple_init_chk_full((mtx_), (class_), (flags_))
#define mutex_simple_init_auto_internal(mtx_, flags_)                          \
    do {                                                                       \
        static const struct lock_chk_class __auto_class = {                    \
            .name = #mtx_,                                                     \
            .file = __RELFILE__,                                               \
            .line = __LINE__,                                                  \
        };                                                                     \
        mutex_simple_init_chk_full((mtx_), &__auto_class, (flags_));           \
    } while (0)

#else /* !defined(DEBUG_LOCK_CHK) */

#define MUTEX_SIMPLE_INIT_CHK(id_, class_, flags_)                             \
    ((struct mutex_simple) {                                                   \
        .owner = NULL,                                                         \
        .waiters =                                                             \
            {                                                                  \
                .waiters = LIST_HEAD_INIT((id_).waiters.waiters),              \
                .lock = SPINLOCK_INIT_CHK(NULL, LOCK_UNCHKD),                  \
            },                                                                 \
        .lock = SPINLOCK_INIT_CHK(NULL, LOCK_UNCHKD),                          \
    })

#define mutex_simple_init_chk(mtx_, class_, flags_)                            \
    mutex_simple_init_chk_full((mtx_), NULL, LOCK_UNCHKD)
#define mutex_simple_init_auto_internal(mtx_, flags_)                          \
    mutex_simple_init_chk_full((mtx_), NULL, LOCK_UNCHKD)

#endif /* DEBUG_LOCK_CHK */

#define MUTEX_SIMPLE_INIT(id_)                                                 \
    MUTEX_SIMPLE_INIT_CHK((id_), NULL, LOCK_CHKD_FULL)
#define MUTEX_SIMPLE_DEFINE(id) struct mutex_simple id = MUTEX_SIMPLE_INIT(id)
#define MUTEX_SIMPLE_DEFINE_CHK(id, class_, flags_)                            \
    struct mutex_simple id = MUTEX_SIMPLE_INIT_CHK((id), (class_), (flags_))

#define mutex_simple_init_1(mtx_) mutex_simple_init_2(mtx_, LOCK_CHKD_FULL)
#define mutex_simple_init_2(mtx_, flags_)                                      \
    mutex_simple_init_auto_internal((mtx_), (flags_))
#define mutex_simple_init(...) PP_CALL(mutex_simple_init, __VA_ARGS__)

#define mutex_simple_lock(m_) mutex_simple_lock_full((m_), LOCK_CHK_SITE_HERE())
#define mutex_simple_unlock(m_)                                                \
    mutex_simple_unlock_full((m_), LOCK_CHK_SITE_HERE())
#define mutex_simple_lock_subclass(m_, subclass_)                              \
    mutex_simple_lock_subclass_full((m_), (subclass_), LOCK_CHK_SITE_HERE())

#define MUTEX_SIMPLE_ASSERT_HELD(m)                                            \
    mutex_simple_assert_held_full((m), LOCK_CHK_SITE_HERE())
#define MUTEX_SIMPLE_ASSERT_NOT_HELD(m)                                        \
    mutex_simple_assert_not_held_full((m), LOCK_CHK_SITE_HERE())

static inline cc_always_inline cc_unused void
mutex_simple_guard_exit(struct mutex_simple **m) TSA_NO_ANALYSIS {
    if (*m)
        mutex_simple_unlock(*m);
}

static inline cc_always_inline cc_unused void
mutex_simple_guard_assume(struct mutex_simple *m) TSA_ASSERT_CAPABILITY(m) {
    cc_ignore(m);
}

static inline cc_always_inline cc_unused struct mutex_simple *
mutex_simple_guard_enter(struct mutex_simple *m) TSA_MAY_SLEEP TSA_NO_ANALYSIS {
    mutex_simple_lock(m);
    return m;
}

static inline cc_always_inline cc_unused struct mutex_simple *
mutex_simple_guard_subclass_enter(struct mutex_simple *m,
                                  enum lock_subclass subclass)
    TSA_MAY_SLEEP TSA_NO_ANALYSIS {
    mutex_simple_lock_subclass(m, subclass);
    return m;
}

#define mutex_simple_guard(m_)                                                 \
    cc_cleanup(mutex_simple_guard_exit) struct mutex_simple *PP_CONCAT(        \
        mtx_simple_guard_, __COUNTER__) =                                      \
        (mutex_simple_guard_assume(m_), mutex_simple_guard_enter(m_))

#define mutex_simple_guard_subclass(m_, subclass_)                             \
    cc_cleanup(mutex_simple_guard_exit) struct mutex_simple *PP_CONCAT(        \
        mtx_simple_guard_, __COUNTER__) =                                      \
        (mutex_simple_guard_assume(m_),                                        \
         mutex_simple_guard_subclass_enter((m_), (subclass_)))
