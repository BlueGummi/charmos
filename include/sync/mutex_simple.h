/* @title: Simple Mutex */
#pragma once
#include <stdbool.h>
#include <sync/lock_chk_types.h>
#include <sync/spinlock.h>
#include <thread/queue.h>

struct mutex_simple {
    struct thread *owner;
    struct thread_queue waiters;
    struct spinlock lock;

#ifdef DEBUG_LOCK_CHK
    enum lock_chk_flags chk_flags;
    bool chk_initialized;
    _Atomic bool chk_used;
    struct lock_chk_map chk_map;
#endif /* DEBUG_LOCK_CHK */
};

void mutex_simple_init_chk_internal(struct mutex_simple *m,
                                    const struct lock_chk_class *class,
                                    enum lock_chk_flags flags);
void mutex_simple_set_chk_flags(struct mutex_simple *m,
                                enum lock_chk_flags flags);
void mutex_simple_reinit_chk(struct mutex_simple *m,
                             const struct lock_chk_class *class,
                             enum lock_chk_flags flags);
void mutex_simple_lock_internal(struct mutex_simple *m,
                                const struct lock_chk_site *site);
void mutex_simple_unlock_internal(struct mutex_simple *m,
                                  const struct lock_chk_site *site);
void mutex_simple_lock_subclass_internal(struct mutex_simple *m,
                                         unsigned int subclass,
                                         const struct lock_chk_site *site);

#ifdef DEBUG_LOCK_CHK

#define MUTEX_SIMPLE_INIT_CHK(id_, class_, flags_)                             \
    ((struct mutex_simple) {                                                   \
        .owner = NULL,                                                         \
        .waiters =                                                             \
            {                                                                  \
                .list = LIST_HEAD_INIT((id_).waiters.list),                    \
                .lock = SPINLOCK_INIT_CHK(NULL, LOCK_UNCHKD),                  \
            },                                                                 \
        .lock = SPINLOCK_INIT_CHK(NULL, LOCK_UNCHKD),                          \
        .chk_flags = (flags_),                                                 \
        .chk_initialized = true,                                               \
        .chk_used = ATOMIC_VAR_INIT(false),                                    \
        .chk_map = LOCK_CHK_MAP_VALUE_INIT(class_),                            \
    })

#define mutex_simple_init_chk(mtx_, class_, flags_)                            \
    mutex_simple_init_chk_internal((mtx_), (class_), (flags_))
#define mutex_simple_init_auto_internal(mtx_, flags_)                          \
    do {                                                                       \
        static const struct lock_chk_class __auto_class = {                    \
            .name = #mtx_,                                                     \
            .file = __RELFILE__,                                               \
            .line = __LINE__,                                                  \
        };                                                                     \
        mutex_simple_init_chk_internal((mtx_), &__auto_class, (flags_));       \
    } while (0)

#else /* !defined(DEBUG_LOCK_CHK) */

#define MUTEX_SIMPLE_INIT_CHK(id_, class_, flags_)                             \
    ((struct mutex_simple) {                                                   \
        .owner = NULL,                                                         \
        .waiters =                                                             \
            {                                                                  \
                .list = LIST_HEAD_INIT((id_).waiters.list),                    \
                .lock = SPINLOCK_INIT_CHK(NULL, LOCK_UNCHKD),                  \
            },                                                                 \
        .lock = SPINLOCK_INIT_CHK(NULL, LOCK_UNCHKD),                          \
    })

#define mutex_simple_init_chk(mtx_, class_, flags_)                            \
    mutex_simple_init_chk_internal((mtx_), NULL, LOCK_UNCHKD)
#define mutex_simple_init_auto_internal(mtx_, flags_)                          \
    mutex_simple_init_chk_internal((mtx_), NULL, LOCK_UNCHKD)

#endif /* DEBUG_LOCK_CHK */

#define MUTEX_SIMPLE_INIT(id_)                                                 \
    MUTEX_SIMPLE_INIT_CHK((id_), NULL, LOCK_CHKD_FULL)
#define MUTEX_SIMPLE_DEFINE(id) struct mutex_simple id = MUTEX_SIMPLE_INIT(id)
#define MUTEX_SIMPLE_DEFINE_CHK(id, class_, flags_)                            \
    struct mutex_simple id = MUTEX_SIMPLE_INIT_CHK((id), (class_), (flags_))

#define mutex_simple_init_1(mtx_)                                              \
    mutex_simple_init_auto_internal((mtx_), LOCK_CHKD_FULL)
#define mutex_simple_init_2(mtx_, flags_)                                      \
    mutex_simple_init_auto_internal((mtx_), (flags_))
#define mutex_simple_init(...)                                                 \
    _DISPATCH(mutex_simple_init, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define mutex_simple_lock(m_)                                                  \
    mutex_simple_lock_internal((m_), LOCK_CHK_SITE_HERE())
#define mutex_simple_unlock(m_)                                                \
    mutex_simple_unlock_internal((m_), LOCK_CHK_SITE_HERE())
#define mutex_simple_lock_subclass(m_, subclass_)                              \
    mutex_simple_lock_subclass_internal((m_), (subclass_), LOCK_CHK_SITE_HERE())
