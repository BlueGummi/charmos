/* @title: Queued Spinlock (MCS-based 4-byte qspinlock) */
#pragma once
#include <asm.h>
#include <bootstage.h>
#include <compiler.h>
#include <console/panic.h>
#include <irq/irq.h>
#include <kassert.h>
#include <sch/irql.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/lock_chk_types.h>

/* TODO: I would really like to use a CNA lock/cohorting as an extension to
 * qspinlock, possibly also with a boot-time flag to enable/disable it
 * for testing. Could be super interesting! */

/*
 * Bit layout of the lock word:
 *
 *  0 -  7: locked byte (Q_SPIN_LOCKED_VAL = 0x01)
 *  8     : pending bit (Q_SPIN_PENDING_VAL = 0x100)
 *  9 - 10: tail context index
 * 16 - 31: tail CPU ID + 1 (16 bits)
 */
#define Q_SPIN_LOCKED_OFFSET 0
#define Q_SPIN_LOCKED_BITS 8
#define Q_SPIN_LOCKED_MASK 0x000000FFU
#define Q_SPIN_LOCKED_VAL 0x00000001U

#define Q_SPIN_PENDING_OFFSET 8
#define Q_SPIN_PENDING_BITS 1
#define Q_SPIN_PENDING_MASK 0x00000100U
#define Q_SPIN_PENDING_VAL 0x00000100U

#define Q_SPIN_LOCKED_PENDING_MASK (Q_SPIN_LOCKED_MASK | Q_SPIN_PENDING_MASK)

#define Q_SPIN_TAIL_LVL_OFFSET 9
#define Q_SPIN_TAIL_LVL_BITS 2
#define Q_SPIN_TAIL_LVL_MASK 0x00000600U

#define Q_SPIN_TAIL_CPU_OFFSET 16
#define Q_SPIN_TAIL_CPU_BITS 16
#define Q_SPIN_TAIL_CPU_MASK 0xFFFF0000U

#define Q_SPIN_TAIL_MASK (Q_SPIN_TAIL_LVL_MASK | Q_SPIN_TAIL_CPU_MASK)

enum qspinlock_level {
    QSPINLOCK_LEVEL_NORMAL, /* prev irql = DISPATCH */
    QSPINLOCK_LEVEL_IRQ,    /* prev irql = HIGH */
    QSPINLOCK_LEVEL_NMI,    /* For later usage */
    QSPINLOCK_LEVEL_MAX,
};

struct qspinlock {
    _Atomic uint32_t val;

#ifdef DEBUG_LOCK_CHK
    enum lock_chk_flags chk_flags;
    bool chk_initialized;
    _Atomic bool chk_used;
    _Atomic uint8_t irq_usage;
    struct lock_chk_map chk_map;
#endif /* DEBUG_LOCK_CHK */
};

#define QSPINLOCK_INIT QSPINLOCK_INIT_CHK(NULL, LOCK_CHKD_FULL)
#define QSPINLOCK_DEFINE(id) struct qspinlock id = QSPINLOCK_INIT
#define QSPINLOCK_DEFINE_CHK(id, class_, flags_)                               \
    struct qspinlock id = QSPINLOCK_INIT_CHK((class_), (flags_))

static inline void
qspinlock_init_chk_internal(struct qspinlock *lock,
                            const struct lock_chk_class *class,
                            enum lock_chk_flags flags);

#ifdef DEBUG_LOCK_CHK

#define __QSPINLOCK_SHALLOW_VALUE_INIT                                         \
    , .irq_usage = ATOMIC_VAR_INIT(LOCK_DEBUG_IRQ_NONE)
#define __QSPINLOCK_POLICY_VALUE_INIT(flags_)                                  \
    , .chk_flags = (flags_), .chk_initialized = true,                          \
        .chk_used = ATOMIC_VAR_INIT(false)
#define __QSPINLOCK_MAP_VALUE_INIT(class_)                                     \
    , .chk_map = LOCK_CHK_MAP_VALUE_INIT(class_)
#define QSPINLOCK_INIT_CHK(class_, flags_)                                     \
    ((struct qspinlock) {                                                      \
        .val = ATOMIC_VAR_INIT(0) __QSPINLOCK_POLICY_VALUE_INIT(flags_)        \
            __QSPINLOCK_SHALLOW_VALUE_INIT __QSPINLOCK_MAP_VALUE_INIT(         \
                class_)})

#define qspinlock_init_chk(lock_, class_, flags_)                              \
    qspinlock_init_chk_internal((lock_), (class_), (flags_))
#define qspinlock_init_auto_internal(lock_, flags_)                            \
    do {                                                                       \
        static const struct lock_chk_class __auto_class = {                    \
            .name = #lock_,                                                    \
            .file = __RELFILE__,                                               \
            .line = __LINE__,                                                  \
        };                                                                     \
        qspinlock_init_chk_internal((lock_), &__auto_class, (flags_));         \
    } while (0)

static inline void qspinlock_policy_init_internal(struct qspinlock *lock,
                                                  enum lock_chk_flags flags) {
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    lock->chk_flags = flags;
    lock->chk_initialized = true;
    atomic_store_explicit(&lock->chk_used, false, memory_order_relaxed);
}

static inline void
qspinlock_map_init_internal(struct qspinlock *lock,
                            const struct lock_chk_class *class,
                            enum lock_chk_flags flags) {
    kassert(flags == LOCK_UNCHKD || class != NULL);
    lock_chk_map_runtime_init(&lock->chk_map, class);
}

static inline void qspinlock_shallow_init_internal(struct qspinlock *lock) {
    atomic_store_explicit(&lock->irq_usage, LOCK_DEBUG_IRQ_NONE,
                          memory_order_relaxed);
}

static inline void qspinlock_set_chk_flags(struct qspinlock *lock,
                                           enum lock_chk_flags flags) {
    kassert(lock->chk_initialized);
    kassert(!qspin_held(lock));
    kassert(!atomic_load_explicit(&lock->chk_used, memory_order_relaxed));
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    lock->chk_flags = flags;
}

static inline void qspinlock_reinit_chk(struct qspinlock *lock,
                                        const struct lock_chk_class *class,
                                        enum lock_chk_flags flags) {
    kassert(lock->chk_initialized);
    kassert(!qspin_held(lock));
    qspinlock_init_chk_internal(lock, class, flags);
}

static inline void qspinlock_note_use(struct qspinlock *lock,
                                      bool raw_operation) {
    lock_chk_note_lock_use(lock->chk_initialized, lock->chk_flags,
                           &lock->chk_used, true, raw_operation);
}

static inline bool qspinlock_order_checked(struct qspinlock *lock) {
    return lock_chk_tracking_active() &&
           (lock->chk_flags & LOCK_CHKD_ORDER) != 0;
}

static inline void qspinlock_classify(struct qspinlock *lock,
                                      enum lock_debug_irq_usage usage,
                                      const struct lock_chk_site *site) {
    lock_debug_spin_classify(&lock->irq_usage, usage, lock, LOCK_CHK_TYPE_QSPIN,
                             site);
}

static inline bool qspinlock_deep_checked(struct qspinlock *lock) {
    return lock_chk_tracking_active() && lock->chk_flags != LOCK_UNCHKD;
}

static inline struct lock_chk_acquire_request qspinlock_chk_acquire_request(
    struct qspinlock *lock, const struct lock_chk_site *site,
    enum lock_chk_wait_kind wait_kind, unsigned int subclass,
    bool raw_operation, bool irq_safe) {
    return lock_chk_acquire_request_make(
        &lock->chk_map, lock, site, lock->chk_flags, LOCK_CHK_TYPE_QSPIN,
        LOCK_CHK_MODE_EXCLUSIVE, wait_kind, subclass, raw_operation, irq_safe);
}

static inline struct lock_chk_release_request
qspinlock_chk_release_request(struct qspinlock *lock,
                              const struct lock_chk_site *site) {
    return lock_chk_release_request_make(&lock->chk_map, lock, site,
                                         lock->chk_flags, LOCK_CHK_TYPE_QSPIN,
                                         LOCK_CHK_MODE_EXCLUSIVE);
}

#else /* !defined(DEBUG_LOCK_CHK) */

#define __QSPINLOCK_SHALLOW_VALUE_INIT
#define __QSPINLOCK_POLICY_VALUE_INIT(flags_)
#define __QSPINLOCK_MAP_VALUE_INIT(class_)
#define QSPINLOCK_INIT_CHK(class_, flags_)                                     \
    ((struct qspinlock) {.val = ATOMIC_VAR_INIT(0)})

#define qspinlock_init_chk(lock_, class_, flags_)                              \
    qspinlock_init_chk_internal((lock_), NULL, LOCK_UNCHKD)
#define qspinlock_init_auto_internal(lock_, flags_)                            \
    qspinlock_init_chk_internal((lock_), NULL, LOCK_UNCHKD)

static inline void qspinlock_policy_init_internal(struct qspinlock *lock,
                                                  enum lock_chk_flags flags) {
    unused(lock, flags);
}

static inline void
qspinlock_map_init_internal(struct qspinlock *lock,
                            const struct lock_chk_class *class,
                            enum lock_chk_flags flags) {
    unused(lock, class, flags);
}

static inline void qspinlock_shallow_init_internal(struct qspinlock *lock) {
    unused(lock);
}

static inline void qspinlock_set_chk_flags(struct qspinlock *lock,
                                           enum lock_chk_flags flags) {
    unused(lock, flags);
}

static inline void qspinlock_reinit_chk(struct qspinlock *lock,
                                        const struct lock_chk_class *class,
                                        enum lock_chk_flags flags) {
    qspinlock_init_chk_internal(lock, class, flags);
}

static inline void qspinlock_note_use(struct qspinlock *lock,
                                      bool raw_operation) {
    unused(lock, raw_operation);
}

static inline bool qspinlock_order_checked(struct qspinlock *lock) {
    unused(lock);
    return false;
}

static inline void qspinlock_classify(struct qspinlock *lock,
                                      enum lock_debug_irq_usage usage,
                                      const struct lock_chk_site *site) {
    unused(lock, usage, site);
}

static inline bool qspinlock_deep_checked(struct qspinlock *lock) {
    unused(lock);
    return false;
}

#endif /* DEBUG_LOCK_CHK */

static inline void
qspinlock_init_chk_internal(struct qspinlock *lock,
                            const struct lock_chk_class *class,
                            enum lock_chk_flags flags) {
    atomic_store_explicit(&lock->val, 0, memory_order_relaxed);
    qspinlock_policy_init_internal(lock, flags);
    qspinlock_shallow_init_internal(lock);
    qspinlock_map_init_internal(lock, class, flags);
}

#define qspinlock_init_1(lock_)                                                \
    qspinlock_init_auto_internal((lock_), LOCK_CHKD_FULL)
#define qspinlock_init_2(lock_, flags_)                                        \
    qspinlock_init_auto_internal((lock_), (flags_))
#define qspinlock_init(...)                                                    \
    _DISPATCH(qspinlock_init, PP_NARG(__VA_ARGS__))(__VA_ARGS__)
#define qspin_init(...) qspinlock_init(__VA_ARGS__)
#define qspin_init_chk(...) qspinlock_init_chk(__VA_ARGS__)

static inline bool
    __warn_unused_result qspin_trylock_physical(struct qspinlock *lock) {
    uint32_t expected = 0;
    return atomic_compare_exchange_strong_explicit(
        &lock->val, &expected, Q_SPIN_LOCKED_VAL, memory_order_acquire,
        memory_order_relaxed);
}

void qspin_lock_slowpath(struct qspinlock *lock, uint32_t val);

static inline void qspin_lock_physical(struct qspinlock *lock) {
    uint32_t val = 0;
    if (likely(atomic_compare_exchange_strong_explicit(
            &lock->val, &val, Q_SPIN_LOCKED_VAL, memory_order_acquire,
            memory_order_relaxed)))
        return;

    qspin_lock_slowpath(lock, val);
}

static inline void qspin_unlock_physical(struct qspinlock *lock) {
    /* Clear only the locked byte so pending and tail bits remain valid */
    atomic_store_explicit((_Atomic uint8_t *) &lock->val, 0,
                          memory_order_release);
}

static inline bool qspin_is_locked(const struct qspinlock *lock) {
    return (atomic_load_explicit(&lock->val, memory_order_relaxed) &
            Q_SPIN_LOCKED_MASK) != 0;
}

static inline bool qspin_held(const struct qspinlock *lock) {
    return qspin_is_locked(lock);
}

static inline void qspinlock_restore_interrupts(bool enabled) {
    if (enabled)
        enable_interrupts();
}

static inline bool __warn_unused_result qspin_trylock_raw_internal(
    struct qspinlock *lock, const struct lock_chk_site *site) {
    qspinlock_note_use(lock, true);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = qspinlock_deep_checked(lock);
    if (checked_deep) {
        req = qspinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_TRY, 0,
                                            true, false);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    if (qspin_trylock_physical(lock)) {

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

static inline void qspin_lock_raw_internal(struct qspinlock *lock,
                                           const struct lock_chk_site *site) {
    qspinlock_note_use(lock, true);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = qspinlock_deep_checked(lock);
    if (checked_deep) {
        req = qspinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_BLOCKING,
                                            0, true, false);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    qspin_lock_physical(lock);

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_acquired(&token);
#endif
}

static inline void qspin_unlock_raw_internal(struct qspinlock *lock,
                                             const struct lock_chk_site *site) {

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_release_request req;
    struct lock_chk_release_token token;
    bool checked_deep = qspinlock_deep_checked(lock);
    if (checked_deep) {
        req = qspinlock_chk_release_request(lock, site);
        lock_chk_before_release(&token, &req);
    }
#endif

    qspin_unlock_physical(lock);

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_released(&token);
#endif
}

static inline void qspin_unlock_internal(struct qspinlock *lock,
                                         enum irql old_irql,
                                         const struct lock_chk_site *site) {
#ifdef DEBUG_LOCK_CHK
    bool checked_shallow = qspinlock_order_checked(lock);
    struct lock_chk_release_request req;
    struct lock_chk_release_token token;
    bool checked_deep = qspinlock_deep_checked(lock);
#endif

    bool irqs_enabled = are_interrupts_enabled();
    disable_interrupts();

#ifdef DEBUG_LOCK_CHK
    if (checked_shallow)
        lock_debug_spin_validate_top(lock, LOCK_CHK_TYPE_QSPIN, old_irql, site);
    if (checked_deep) {
        req = qspinlock_chk_release_request(lock, site);
        lock_chk_before_release(&token, &req);
    }
#endif

    qspin_unlock_physical(lock);

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_released(&token);
    if (checked_shallow)
        lock_debug_spin_pop(lock, LOCK_CHK_TYPE_QSPIN);
#endif

    qspinlock_restore_interrupts(irqs_enabled);

    irql_lower(old_irql);
}

static inline enum irql __warn_unused_result
qspin_lock_subclass_internal(struct qspinlock *lock, unsigned int subclass,
                             const struct lock_chk_site *site) {
    kassert(subclass < LOCK_CHK_MAX_SUBCLASSES);
    if (bootstage_get() >= BOOTSTAGE_MID_MP &&
        (irq_in_interrupt() || irq_in_nmi()))
        panic(
            "Attempted to take non-ISR safe qspinlock outside thread context");

    qspinlock_note_use(lock, false);
    bool checked_shallow = qspinlock_order_checked(lock);
    if (checked_shallow)
        qspinlock_classify(lock, LOCK_DEBUG_IRQ_DISPATCH, site);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = qspinlock_deep_checked(lock);
    if (checked_deep) {
        req = qspinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_BLOCKING,
                                            subclass, false, false);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    qspin_lock_physical(lock);

    bool irqs_enabled = are_interrupts_enabled();
    disable_interrupts();

#ifdef DEBUG_LOCK_CHK
    if (checked_shallow)
        lock_debug_spin_push(lock, LOCK_CHK_TYPE_QSPIN, irql, site);

    if (checked_deep)
        lock_chk_acquired(&token);
#endif

    qspinlock_restore_interrupts(irqs_enabled);

    return irql;
}

static inline enum irql __warn_unused_result
qspin_lock_internal(struct qspinlock *lock, const struct lock_chk_site *site) {
    return qspin_lock_subclass_internal(lock, 0, site);
}

static inline enum irql __warn_unused_result qspin_lock_irq_disable_internal(
    struct qspinlock *lock, const struct lock_chk_site *site) {
    if (bootstage_get() >= BOOTSTAGE_MID_MP && irq_in_nmi())
        panic("Attempted to take non-raw qspinlock from an NMI");

    qspinlock_note_use(lock, false);
    bool checked_shallow = qspinlock_order_checked(lock);
    if (checked_shallow)
        qspinlock_classify(lock, LOCK_DEBUG_IRQ_HIGH, site);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = qspinlock_deep_checked(lock);
    if (checked_deep) {
        req = qspinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_BLOCKING,
                                            0, false, true);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    enum irql irql = irql_raise(IRQL_HIGH_LEVEL);
    qspin_lock_physical(lock);

#ifdef DEBUG_LOCK_CHK
    if (checked_shallow)
        lock_debug_spin_push(lock, LOCK_CHK_TYPE_QSPIN, irql, site);

    if (checked_deep)
        lock_chk_acquired(&token);
#endif

    return irql;
}

static inline bool __warn_unused_result qspin_trylock_internal(
    struct qspinlock *lock, enum irql *out, const struct lock_chk_site *site) {
    if (bootstage_get() >= BOOTSTAGE_MID_MP &&
        (irq_in_interrupt() || irq_in_nmi()))
        panic(
            "Attempted to take non-ISR safe qspinlock outside thread context");

    qspinlock_note_use(lock, false);
    bool checked_shallow = qspinlock_order_checked(lock);
    if (checked_shallow)
        qspinlock_classify(lock, LOCK_DEBUG_IRQ_DISPATCH, site);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = qspinlock_deep_checked(lock);
    if (checked_deep) {
        req = qspinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_TRY, 0,
                                            false, false);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    *out = irql_raise(IRQL_DISPATCH_LEVEL);
    if (qspin_trylock_physical(lock)) {
        bool irqs_enabled = are_interrupts_enabled();
        disable_interrupts();
#ifdef DEBUG_LOCK_CHK
        if (checked_shallow)
            lock_debug_spin_push(lock, LOCK_CHK_TYPE_QSPIN, *out, site);

        if (checked_deep)
            lock_chk_acquired(&token);
#endif
        qspinlock_restore_interrupts(irqs_enabled);
        return true;
    }

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_cancel(&token);
#endif

    irql_lower(*out);
    return false;
}

static inline bool __warn_unused_result qspin_trylock_irq_disable_internal(
    struct qspinlock *lock, enum irql *out, const struct lock_chk_site *site) {
    if (bootstage_get() >= BOOTSTAGE_MID_MP && irq_in_nmi())
        panic("Attempted to take non-raw qspinlock from an NMI");

    qspinlock_note_use(lock, false);
    bool checked_shallow = qspinlock_order_checked(lock);
    if (checked_shallow)
        qspinlock_classify(lock, LOCK_DEBUG_IRQ_HIGH, site);

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_acquire_request req;
    struct lock_chk_acquire_token token;
    bool checked_deep = qspinlock_deep_checked(lock);
    if (checked_deep) {
        req = qspinlock_chk_acquire_request(lock, site, LOCK_CHK_WAIT_TRY, 0,
                                            false, true);
        lock_chk_before_acquire(&token, &req);
    }
#endif

    *out = irql_raise(IRQL_HIGH_LEVEL);
    if (qspin_trylock_physical(lock)) {

#ifdef DEBUG_LOCK_CHK
        if (checked_shallow)
            lock_debug_spin_push(lock, LOCK_CHK_TYPE_QSPIN, *out, site);

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

#define qspin_lock(lock_) qspin_lock_internal((lock_), LOCK_CHK_SITE_HERE())
#define qspin_lock_subclass(lock_, subclass_)                                  \
    qspin_lock_subclass_internal((lock_), (subclass_), LOCK_CHK_SITE_HERE())
#define qspin_lock_irq_disable(lock_)                                          \
    qspin_lock_irq_disable_internal((lock_), LOCK_CHK_SITE_HERE())
#define qspin_unlock(lock_, old_)                                              \
    qspin_unlock_internal((lock_), (old_), LOCK_CHK_SITE_HERE())
#define qspin_unlock_irq_restore(lock_, old_)                                  \
    qspin_unlock_internal((lock_), (old_), LOCK_CHK_SITE_HERE())
#define qspin_trylock(lock_, out_)                                             \
    qspin_trylock_internal((lock_), (out_), LOCK_CHK_SITE_HERE())
#define qspin_trylock_irq_disable(lock_, out_)                                 \
    qspin_trylock_irq_disable_internal((lock_), (out_), LOCK_CHK_SITE_HERE())
#define qspin_lock_raw(lock_)                                                  \
    qspin_lock_raw_internal((lock_), LOCK_CHK_SITE_HERE())
#define qspin_trylock_raw(lock_)                                               \
    qspin_trylock_raw_internal((lock_), LOCK_CHK_SITE_HERE())
#define qspin_unlock_raw(lock_)                                                \
    qspin_unlock_raw_internal((lock_), LOCK_CHK_SITE_HERE())

#define QSPINLOCK_ASSERT_HELD(l) kassert(qspin_is_locked(l))
