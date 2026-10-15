/* @title: Mutex */
#pragma once
#include <compiler/core.h>
#include <sch/irql.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/lock_chk_types.h>
#include <tsa.h>

/* mutex: pointer sized mutex
 *
 *      ┌─────────────────────────┐
 * Bits │  ....  ....  ....  3..0 │
 * Use  │  %%%%  %%%%  %%%%  %rrh │
 *      └─────────────────────────┘
 *
 * h - "held" - is the lock held?
 *
 * r - reserved for future use
 *
 * %%%% - pointer to owner thread
 *
 */

struct TSA_CAPABILITY("mutex") mutex {
    atomic_uintptr_t lock_word;

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_lock chk;
#endif /* DEBUG_LOCK_CHK */
};

void mutex_init_chk_full(struct mutex *mtx, const struct lock_chk_class *class,
                         enum lock_chk_flags flags);
void mutex_set_chk_flags(struct mutex *mtx, enum lock_chk_flags flags);
void mutex_reinit_chk(struct mutex *mtx, const struct lock_chk_class *class,
                      enum lock_chk_flags flags);
void mutex_unlock_full(struct mutex *mutex, const struct lock_chk_site *site)
    TSA_RELEASES(mutex);

void mutex_lock_full(struct mutex *mutex, const struct lock_chk_site *site)
    TSA_ACQUIRES(mutex) TSA_MAY_SLEEP;

void mutex_lock_subclass_full(struct mutex *mutex, enum lock_subclass subclass,
                              const struct lock_chk_site *site)
    TSA_ACQUIRES(mutex) TSA_MAY_SLEEP;

bool mutex_locked(struct mutex *mtx);
struct thread *mutex_read_owner(struct mutex *mtx);
void mutex_assert_held_full(struct mutex *mtx, const struct lock_chk_site *site)
    TSA_ASSERT_CAPABILITY(mtx);

void mutex_assert_not_held_full(struct mutex *mtx,
                                const struct lock_chk_site *site);

#ifdef DEBUG_LOCK_CHK

#define MUTEX_INIT_CHK(class_, flags_)                                         \
    ((struct mutex) {                                                          \
        .lock_word = ATOMIC_VAR_INIT(0),                                       \
        .chk = LOCK_CHK_LOCK_VALUE_INIT((class_), (flags_)),                   \
    })

#define mutex_init_chk(mtx_, class_, flags_)                                   \
    mutex_init_chk_full((mtx_), (class_), (flags_))
#define mutex_init_auto_internal(mtx_, flags_)                                 \
    do {                                                                       \
        static const struct lock_chk_class __auto_class = {                    \
            .name = #mtx_,                                                     \
            .file = __RELFILE__,                                               \
            .line = __LINE__,                                                  \
        };                                                                     \
        mutex_init_chk_full((mtx_), &__auto_class, (flags_));                  \
    } while (0)

#else /* !defined(DEBUG_LOCK_CHK) */

#define MUTEX_INIT_CHK(class_, flags_)                                         \
    ((struct mutex) {.lock_word = ATOMIC_VAR_INIT(0)})

#define mutex_init_chk(mtx_, class_, flags_)                                   \
    mutex_init_chk_full((mtx_), NULL, LOCK_UNCHKD)
#define mutex_init_auto_internal(mtx_, flags_)                                 \
    mutex_init_chk_full((mtx_), NULL, LOCK_UNCHKD)

#endif /* DEBUG_LOCK_CHK */

#define MUTEX_INIT MUTEX_INIT_CHK(NULL, LOCK_CHKD_FULL)
#define MUTEX_DEFINE(id) struct mutex id = MUTEX_INIT
#define MUTEX_DEFINE_CHK(id, class_, flags_)                                   \
    struct mutex id = MUTEX_INIT_CHK((class_), (flags_))

#define mutex_init_1(mtx_) mutex_init_2(mtx_, LOCK_CHKD_FULL)
#define mutex_init_2(mtx_, flags_) mutex_init_auto_internal((mtx_), (flags_))
#define mutex_init(...) PP_CALL(mutex_init, __VA_ARGS__)

#define mutex_lock(mutex_) mutex_lock_full((mutex_), LOCK_CHK_SITE_HERE())
#define mutex_lock_subclass(mutex_, subclass_)                                 \
    mutex_lock_subclass_full((mutex_), (subclass_), LOCK_CHK_SITE_HERE())
#define mutex_unlock(mutex_) mutex_unlock_full((mutex_), LOCK_CHK_SITE_HERE())

#define MUTEX_ASSERT_HELD(m) mutex_assert_held_full((m), LOCK_CHK_SITE_HERE())
#define MUTEX_ASSERT_NOT_HELD(m)                                               \
    mutex_assert_not_held_full((m), LOCK_CHK_SITE_HERE())

static inline cc_always_inline
cc_unused void mutex_guard_exit(struct mutex **m) TSA_NO_ANALYSIS {
    if (*m)
        mutex_unlock(*m);
}

static inline cc_always_inline cc_unused void
mutex_guard_assume(struct mutex *m) TSA_ASSERT_CAPABILITY(m) {
    cc_ignore(m);
}

static inline cc_always_inline cc_unused struct mutex *
mutex_guard_enter(struct mutex *m) TSA_MAY_SLEEP TSA_NO_ANALYSIS {
    mutex_lock(m);
    return m;
}

static inline cc_always_inline cc_unused struct mutex *
mutex_guard_subclass_enter(struct mutex *m, enum lock_subclass subclass)
    TSA_MAY_SLEEP TSA_NO_ANALYSIS {
    mutex_lock_subclass(m, subclass);
    return m;
}

#define mutex_guard(m_)                                                        \
    cc_cleanup(mutex_guard_exit) struct mutex *PP_CONCAT(mtx_guard_,           \
                                                         __COUNTER__) =        \
        (mutex_guard_assume(m_), mutex_guard_enter(m_))

#define mutex_guard_subclass(m_, subclass_)                                    \
    cc_cleanup(mutex_guard_exit) struct mutex *PP_CONCAT(mtx_guard_,           \
                                                         __COUNTER__) =        \
        (mutex_guard_assume(m_),                                               \
         mutex_guard_subclass_enter((m_), (subclass_)))
