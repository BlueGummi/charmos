#pragma once
#include <asm.h>
#include <bootstage.h>
#include <compiler.h>
#include <console/panic.h>
#include <irq/irq.h>
#include <kassert.h>
#include <sch/irql.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sync/lock_chk_types.h>
#include <sync/raw_spinlock.h>

struct spinlock {
    struct raw_spinlock raw;

#ifdef DEBUG_LOCK_CHK
    enum lock_chk_flags chk_flags;
    bool chk_initialized;
    _Atomic bool chk_used;
    _Atomic uint8_t irq_usage;
    struct lock_chk_map chk_map;
#endif /* DEBUG_LOCK_CHK */
};

#define SPINLOCK_INIT SPINLOCK_INIT_CHK(NULL, LOCK_CHKD_FULL)
#define SPINLOCK_DEFINE(id) struct spinlock id = SPINLOCK_INIT
#define SPINLOCK_DEFINE_CHK(id, class_, flags_)                                \
    struct spinlock id = SPINLOCK_INIT_CHK((class_), (flags_))

#define spinlock_init_1(lock_)                                                 \
    spinlock_init_auto_internal((lock_), LOCK_CHKD_FULL)
#define spinlock_init_2(lock_, flags_)                                         \
    spinlock_init_auto_internal((lock_), (flags_))
#define spinlock_init(...)                                                     \
    _DISPATCH(spinlock_init, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

static inline void
spinlock_init_chk_internal(struct spinlock *lock,
                           const struct lock_chk_class *class,
                           enum lock_chk_flags flags);

#ifdef DEBUG_LOCK_CHK

#define __SPINLOCK_POLICY_VALUE_INIT(flags_)                                   \
    , .chk_flags = (flags_), .chk_initialized = true,                          \
        .chk_used = ATOMIC_VAR_INIT(false)
#define __SPINLOCK_SHALLOW_VALUE_INIT                                          \
    , .irq_usage = ATOMIC_VAR_INIT(LOCK_DEBUG_IRQ_NONE)
#define __SPINLOCK_MAP_VALUE_INIT(class_)                                      \
    , .chk_map = LOCK_CHK_MAP_VALUE_INIT(class_)
#define SPINLOCK_INIT_CHK(class_, flags_)                                      \
    ((struct spinlock) {                                                       \
        .raw = RAW_SPINLOCK_INIT __SPINLOCK_POLICY_VALUE_INIT(flags_)          \
            __SPINLOCK_SHALLOW_VALUE_INIT __SPINLOCK_MAP_VALUE_INIT(class_)})

#define spinlock_init_chk(lock_, class_, flags_)                               \
    spinlock_init_chk_internal((lock_), (class_), (flags_))
#define spinlock_init_auto_internal(lock_, flags_)                             \
    do {                                                                       \
        static const struct lock_chk_class __auto_class = {                    \
            .name = #lock_,                                                    \
            .file = __RELFILE__,                                               \
            .line = __LINE__,                                                  \
        };                                                                     \
        spinlock_init_chk_internal((lock_), &__auto_class, (flags_));          \
    } while (0)

static inline void spinlock_policy_init_internal(struct spinlock *lock,
                                                 enum lock_chk_flags flags) {
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    lock->chk_flags = flags;
    lock->chk_initialized = true;
    atomic_store_explicit(&lock->chk_used, false, memory_order_relaxed);
}
static inline void spinlock_shallow_init_internal(struct spinlock *lock) {
    atomic_store_explicit(&lock->irq_usage, LOCK_DEBUG_IRQ_NONE,
                          memory_order_relaxed);
}
static inline void
spinlock_map_init_internal(struct spinlock *lock,
                           const struct lock_chk_class *class,
                           enum lock_chk_flags flags) {
    kassert(flags == LOCK_UNCHKD || class != NULL);
    lock_chk_map_runtime_init(&lock->chk_map, class);
}

static inline void spinlock_note_use(struct spinlock *lock,
                                     bool raw_operation) {
    lock_chk_note_lock_use(lock->chk_initialized, lock->chk_flags,
                           &lock->chk_used, true, raw_operation);
}

static inline bool spinlock_order_checked(struct spinlock *lock) {
    return lock_chk_tracking_active() &&
           (lock->chk_flags & LOCK_CHKD_ORDER) != 0;
}

static inline void spinlock_classify(struct spinlock *lock,
                                     enum lock_debug_irq_usage usage,
                                     const struct lock_chk_site *site) {
    lock_debug_spin_classify(&lock->irq_usage, usage, lock, LOCK_CHK_TYPE_SPIN,
                             site);
}

static inline bool spinlock_deep_checked(struct spinlock *lock) {
    return lock_chk_tracking_active() && lock->chk_flags != LOCK_UNCHKD;
}

static inline struct lock_chk_acquire_request spinlock_chk_acquire_request(
    struct spinlock *lock, const struct lock_chk_site *site,
    enum lock_chk_wait_kind wait_kind, unsigned int subclass,
    bool raw_operation, bool irq_safe) {
    return lock_chk_acquire_request_make(
        &lock->chk_map, lock, site, lock->chk_flags, LOCK_CHK_TYPE_SPIN,
        LOCK_CHK_MODE_EXCLUSIVE, wait_kind, subclass, raw_operation, irq_safe);
}

static inline struct lock_chk_release_request
spinlock_chk_release_request(struct spinlock *lock,
                             const struct lock_chk_site *site) {
    return lock_chk_release_request_make(&lock->chk_map, lock, site,
                                         lock->chk_flags, LOCK_CHK_TYPE_SPIN,
                                         LOCK_CHK_MODE_EXCLUSIVE);
}

#else /* !defined(DEBUG_LOCK_CHK) */

#define __SPINLOCK_POLICY_VALUE_INIT(flags_)
#define __SPINLOCK_SHALLOW_VALUE_INIT
#define __SPINLOCK_MAP_VALUE_INIT(class_)
#define SPINLOCK_INIT_CHK(class_, flags_)                                      \
    ((struct spinlock) {.raw = RAW_SPINLOCK_INIT})

#define spinlock_init_chk(lock_, class_, flags_)                               \
    spinlock_init_chk_internal((lock_), NULL, LOCK_UNCHKD)
#define spinlock_init_auto_internal(lock_, flags_)                             \
    spinlock_init_chk_internal((lock_), NULL, LOCK_UNCHKD)

static inline void spinlock_policy_init_internal(struct spinlock *lock,
                                                 enum lock_chk_flags flags) {
    unused(lock, flags);
}

static inline void spinlock_shallow_init_internal(struct spinlock *lock) {
    unused(lock);
}

static inline void
spinlock_map_init_internal(struct spinlock *lock,
                           const struct lock_chk_class *class,
                           enum lock_chk_flags flags) {
    unused(lock, class, flags);
}

static inline void spinlock_note_use(struct spinlock *lock,
                                     bool raw_operation) {
    unused(lock, raw_operation);
}

static inline bool spinlock_order_checked(struct spinlock *lock) {
    unused(lock);
    return false;
}

static inline void spinlock_classify(struct spinlock *lock,
                                     enum lock_debug_irq_usage usage,
                                     const struct lock_chk_site *site) {
    unused(lock, usage, site);
}

static inline bool spinlock_deep_checked(struct spinlock *lock) {
    unused(lock);
    return false;
}

#endif /* DEBUG_LOCK_CHK */

static inline bool
    __warn_unused_result spin_trylock_physical(struct spinlock *lock) {
    return raw_spin_trylock(&lock->raw);
}

static inline void spin_lock_physical(struct spinlock *lock) {
    raw_spin_lock(&lock->raw);
}

static inline void spin_unlock_physical(struct spinlock *lock) {
    raw_spin_unlock(&lock->raw);
}

static inline void
spinlock_init_chk_internal(struct spinlock *lock,
                           const struct lock_chk_class *class,
                           enum lock_chk_flags flags) {
    raw_spinlock_init(&lock->raw);
    spinlock_policy_init_internal(lock, flags);
    spinlock_shallow_init_internal(lock);
    spinlock_map_init_internal(lock, class, flags);
}

static inline void spinlock_restore_interrupts(bool enabled) {
    if (enabled)
        enable_interrupts();
}

static inline bool __warn_unused_result spin_trylock_raw_internal(
    struct spinlock *lock, const struct lock_chk_site *site) {
    spinlock_note_use(lock, true);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = spinlock_deep_checked(lock);
    if (checked_deep) {
        req = spinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_TRY, 0,
                                           true, false);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    if (spin_trylock_physical(lock)) {

#ifdef DEBUG_LOCK_CHK
        if (checked_deep)
            lock_chk_acquired(&token);
#endif

        return true;
    }

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_cancel(&token);
#endif

    return false;
}

static inline void spin_lock_raw_internal(struct spinlock *lock,
                                          const struct lock_chk_site *site) {
    spinlock_note_use(lock, true);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = spinlock_deep_checked(lock);
    if (checked_deep) {
        req = spinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_BLOCKING,
                                           0, true, false);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    spin_lock_physical(lock);

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_acquired(&token);
#endif
}

static inline void spin_unlock_raw_internal(struct spinlock *lock,
                                            const struct lock_chk_site *site) {

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_release_request req;
    struct lock_chk_release_token token;
    bool checked_deep = spinlock_deep_checked(lock);
    if (checked_deep) {
        req = spinlock_chk_release_request(lock, site);
        lock_chk_before_release(&token, &req);
    }
#endif

    spin_unlock_physical(lock);

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_released(&token);
#endif
}

static inline void spin_unlock_internal(struct spinlock *lock, enum irql old,
                                        const struct lock_chk_site *site) {

#ifdef DEBUG_LOCK_CHK
    bool checked_shallow = spinlock_order_checked(lock);
    struct lock_chk_release_request req;
    struct lock_chk_release_token token;
    bool checked_deep = spinlock_deep_checked(lock);
#endif

    bool irqs_enabled = are_interrupts_enabled();
    disable_interrupts();

#ifdef DEBUG_LOCK_CHK
    if (checked_shallow)
        lock_debug_spin_validate_top(lock, LOCK_CHK_TYPE_SPIN, old, site);
    if (checked_deep) {
        req = spinlock_chk_release_request(lock, site);
        lock_chk_before_release(&token, &req);
    }
#endif

    spin_unlock_physical(lock);

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_released(&token);
    if (checked_shallow)
        lock_debug_spin_pop(lock, LOCK_CHK_TYPE_SPIN);
#endif

    spinlock_restore_interrupts(irqs_enabled);

    irql_lower(old);
}

static inline enum irql __warn_unused_result
spin_lock_subclass_internal(struct spinlock *lock, unsigned int subclass,
                            const struct lock_chk_site *site) {
    kassert(subclass < LOCK_CHK_MAX_SUBCLASSES);
    if (bootstage_get() >= BOOTSTAGE_MID_MP &&
        (irq_in_interrupt() || irq_in_nmi()))
        panic("Attempted to take non-ISR safe spinlock outside thread context");

    spinlock_note_use(lock, false);
    bool checked_shallow = spinlock_order_checked(lock);
    if (checked_shallow)
        spinlock_classify(lock, LOCK_DEBUG_IRQ_DISPATCH, site);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = spinlock_deep_checked(lock);
    if (checked_deep) {
        req = spinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_BLOCKING,
                                           subclass, false, false);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    spin_lock_physical(lock);

    bool irqs_enabled = are_interrupts_enabled();
    disable_interrupts();

#ifdef DEBUG_LOCK_CHK
    if (checked_shallow)
        lock_debug_spin_push(lock, LOCK_CHK_TYPE_SPIN, irql, site);

    if (checked_deep)
        lock_chk_acquired(&token);
#endif

    spinlock_restore_interrupts(irqs_enabled);

    return irql;
}

static inline enum irql __warn_unused_result
spin_lock_internal(struct spinlock *lock, const struct lock_chk_site *site) {
    return spin_lock_subclass_internal(lock, 0, site);
}

static inline enum irql __warn_unused_result spin_lock_irq_disable_internal(
    struct spinlock *lock, const struct lock_chk_site *site) {
    if (bootstage_get() >= BOOTSTAGE_MID_MP && irq_in_nmi())
        panic("Attempted to take non-raw spinlock from an NMI");

    spinlock_note_use(lock, false);
    bool checked_shallow = spinlock_order_checked(lock);
    if (checked_shallow)
        spinlock_classify(lock, LOCK_DEBUG_IRQ_HIGH, site);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = spinlock_deep_checked(lock);
    if (checked_deep) {
        req = spinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_BLOCKING,
                                           0, false, true);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    enum irql irql = irql_raise(IRQL_HIGH_LEVEL);
    spin_lock_physical(lock);

#ifdef DEBUG_LOCK_CHK
    if (checked_shallow)
        lock_debug_spin_push(lock, LOCK_CHK_TYPE_SPIN, irql, site);

    if (checked_deep)
        lock_chk_acquired(&token);
#endif

    return irql;
}

static inline bool __warn_unused_result spin_trylock_internal(
    struct spinlock *lock, enum irql *out, const struct lock_chk_site *site) {
    if (bootstage_get() >= BOOTSTAGE_MID_MP &&
        (irq_in_interrupt() || irq_in_nmi()))
        panic("Attempted to take non-ISR safe spinlock outside thread context");

    spinlock_note_use(lock, false);
    bool checked_shallow = spinlock_order_checked(lock);
    if (checked_shallow)
        spinlock_classify(lock, LOCK_DEBUG_IRQ_DISPATCH, site);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = spinlock_deep_checked(lock);
    if (checked_deep) {
        req = spinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_TRY, 0,
                                           false, false);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    *out = irql_raise(IRQL_DISPATCH_LEVEL);
    if (spin_trylock_physical(lock)) {
        bool irqs_enabled = are_interrupts_enabled();
        disable_interrupts();

#ifdef DEBUG_LOCK_CHK
        if (checked_shallow)
            lock_debug_spin_push(lock, LOCK_CHK_TYPE_SPIN, *out, site);
        if (checked_deep)
            lock_chk_acquired(&token);
#endif

        spinlock_restore_interrupts(irqs_enabled);
        return true;
    }

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_cancel(&token);
#endif

    irql_lower(*out);
    return false;
}

static inline bool __warn_unused_result spin_trylock_irq_disable_internal(
    struct spinlock *lock, enum irql *out, const struct lock_chk_site *site) {
    if (bootstage_get() >= BOOTSTAGE_MID_MP && irq_in_nmi())
        panic("Attempted to take non-raw spinlock from an NMI");

    spinlock_note_use(lock, false);
    bool checked_shallow = spinlock_order_checked(lock);
    if (checked_shallow)
        spinlock_classify(lock, LOCK_DEBUG_IRQ_HIGH, site);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = spinlock_deep_checked(lock);
    if (checked_deep) {
        req = spinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_TRY, 0,
                                           false, true);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    *out = irql_raise(IRQL_HIGH_LEVEL);
    if (spin_trylock_physical(lock)) {

#ifdef DEBUG_LOCK_CHK
        if (checked_shallow)
            lock_debug_spin_push(lock, LOCK_CHK_TYPE_SPIN, *out, site);
        if (checked_deep)
            lock_chk_acquired(&token);
#endif

        return true;
    }

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_cancel(&token);
#endif
    irql_lower(*out);
    return false;
}

#define spin_lock(lock_) spin_lock_internal((lock_), LOCK_CHK_SITE_HERE())
#define spin_lock_subclass(lock_, subclass_)                                   \
    spin_lock_subclass_internal((lock_), (subclass_), LOCK_CHK_SITE_HERE())
#define spin_lock_irq_disable(lock_)                                           \
    spin_lock_irq_disable_internal((lock_), LOCK_CHK_SITE_HERE())
#define spin_trylock(lock_, out_)                                              \
    spin_trylock_internal((lock_), (out_), LOCK_CHK_SITE_HERE())
#define spin_trylock_irq_disable(lock_, out_)                                  \
    spin_trylock_irq_disable_internal((lock_), (out_), LOCK_CHK_SITE_HERE())
#define spin_unlock(lock_, old_)                                               \
    spin_unlock_internal((lock_), (old_), LOCK_CHK_SITE_HERE())
#define spin_lock_raw(lock_)                                                   \
    spin_lock_raw_internal((lock_), LOCK_CHK_SITE_HERE())
#define spin_trylock_raw(lock_)                                                \
    spin_trylock_raw_internal((lock_), LOCK_CHK_SITE_HERE())
#define spin_unlock_raw(lock_)                                                 \
    spin_unlock_raw_internal((lock_), LOCK_CHK_SITE_HERE())

static inline bool spinlock_held(struct spinlock *lock) {
    return atomic_load(&lock->raw.state);
}

static inline void spinlock_set_chk_flags(struct spinlock *lock,
                                          enum lock_chk_flags flags) {

#ifdef DEBUG_LOCK_CHK
    kassert(lock->chk_initialized);
    kassert(!spinlock_held(lock));
    kassert(!atomic_load_explicit(&lock->chk_used, memory_order_relaxed));
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    lock->chk_flags = flags;
#endif
}

static inline void spinlock_reinit_chk(struct spinlock *lock,
                                       const struct lock_chk_class *class,
                                       enum lock_chk_flags flags) {

#ifdef DEBUG_LOCK_CHK
    kassert(lock->chk_initialized);
    kassert(!spinlock_held(lock));
#endif

    spinlock_init_chk_internal(lock, class, flags);
}

#define SPINLOCK_ASSERT_HELD(l) kassert(spinlock_held(l))
