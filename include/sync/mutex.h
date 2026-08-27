/* @title: Mutex */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <sync/lock_chk_types.h>

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

struct mutex {
    _Atomic(uintptr_t) lock_word;

#ifdef DEBUG_LOCK_CHK
    enum lock_chk_flags chk_flags;
    bool chk_initialized;
    _Atomic bool chk_used;
    struct lock_chk_map chk_map;
#endif /* DEBUG_LOCK_CHK */
};

void mutex_init_chk_internal(struct mutex *mtx,
                             const struct lock_chk_class *class,
                             enum lock_chk_flags flags);
void mutex_set_chk_flags(struct mutex *mtx, enum lock_chk_flags flags);
void mutex_reinit_chk(struct mutex *mtx, const struct lock_chk_class *class,
                      enum lock_chk_flags flags);
void mutex_unlock_internal(struct mutex *mutex,
                           const struct lock_chk_site *site);
void mutex_lock_internal(struct mutex *mutex, const struct lock_chk_site *site);
void mutex_lock_subclass_internal(struct mutex *mutex, unsigned int subclass,
                                  const struct lock_chk_site *site);
bool mutex_held(struct mutex *mtx);
struct thread *mutex_get_owner(struct mutex *mtx);

#ifdef DEBUG_LOCK_CHK

#define MUTEX_INIT_CHK(class_, flags_)                                         \
    ((struct mutex) {                                                          \
        .lock_word = ATOMIC_VAR_INIT(0),                                       \
        .chk_flags = (flags_),                                                 \
        .chk_initialized = true,                                               \
        .chk_used = ATOMIC_VAR_INIT(false),                                    \
        .chk_map = LOCK_CHK_MAP_VALUE_INIT(class_),                            \
    })

#define mutex_init_chk(mtx_, class_, flags_)                                   \
    mutex_init_chk_internal((mtx_), (class_), (flags_))
#define mutex_init_auto_internal(mtx_, flags_)                                 \
    do {                                                                       \
        static const struct lock_chk_class __auto_class = {                    \
            .name = #mtx_,                                                     \
            .file = __RELFILE__,                                               \
            .line = __LINE__,                                                  \
        };                                                                     \
        mutex_init_chk_internal((mtx_), &__auto_class, (flags_));              \
    } while (0)

#else /* !defined(DEBUG_LOCK_CHK) */

#define MUTEX_INIT_CHK(class_, flags_)                                         \
    ((struct mutex) {.lock_word = ATOMIC_VAR_INIT(0)})

#define mutex_init_chk(mtx_, class_, flags_)                                   \
    mutex_init_chk_internal((mtx_), NULL, LOCK_UNCHKD)
#define mutex_init_auto_internal(mtx_, flags_)                                 \
    mutex_init_chk_internal((mtx_), NULL, LOCK_UNCHKD)

#endif /* DEBUG_LOCK_CHK */

#define MUTEX_INIT MUTEX_INIT_CHK(NULL, LOCK_CHKD_FULL)
#define MUTEX_DEFINE(id) struct mutex id = MUTEX_INIT
#define MUTEX_DEFINE_CHK(id, class_, flags_)                                   \
    struct mutex id = MUTEX_INIT_CHK((class_), (flags_))

#define mutex_init_1(mtx_) mutex_init_auto_internal((mtx_), LOCK_CHKD_FULL)
#define mutex_init_2(mtx_, flags_) mutex_init_auto_internal((mtx_), (flags_))
#define mutex_init(...) _DISPATCH(mutex_init, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define mutex_lock(mutex_) mutex_lock_internal((mutex_), LOCK_CHK_SITE_HERE())
#define mutex_lock_subclass(mutex_, subclass_)                                 \
    mutex_lock_subclass_internal((mutex_), (subclass_), LOCK_CHK_SITE_HERE())
#define mutex_unlock(mutex_)                                                   \
    mutex_unlock_internal((mutex_), LOCK_CHK_SITE_HERE())

#define MUTEX_ASSERT_HELD(m) kassert(mutex_held(m))
