/* @title: Queued Spinlock (MCS-based 4-byte qspinlock) */
#pragma once
#include "console/crash.h"
#include <asm.h>
#include <atomic.h>
#include <bootstage.h>
#include <compiler/core.h>
#include <console/panic.h>
#include <enforce.h>
#include <irq/irq.h>
#include <sch/irql.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/lock_chk_types.h>
#include <sync/lock_general.h>

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

struct TSA_CAPABILITY("spinlock") qspinlock {
    atomic_uint32_t val;

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_lock chk;
    atomic(enum lock_op_flags) irq_usage;
#endif /* DEBUG_LOCK_CHK */
};

#define QSPINLOCK_INIT QSPINLOCK_INIT_CHK(NULL, LOCK_CHKD_FULL)
#define QSPINLOCK_DEFINE(id) struct qspinlock id = QSPINLOCK_INIT
#define QSPINLOCK_DEFINE_CHK(id, class_, flags_)                               \
    struct qspinlock id = QSPINLOCK_INIT_CHK((class_), (flags_))

static inline void qspinlock_init_chk_full(struct qspinlock *lock,
                                           const struct lock_chk_class *class,
                                           enum lock_chk_flags flags);
static inline bool qspin_is_locked(const struct qspinlock *lock);

#ifdef DEBUG_LOCK_CHK

#define __QSPINLOCK_SHALLOW_VALUE_INIT                                         \
    , .irq_usage = ATOMIC_VAR_INIT(LOCK_OP_IRQ_NONE)
#define __QSPINLOCK_LOCK_CHK_VALUE_INIT(class_, flags_)                        \
    , .chk = LOCK_CHK_LOCK_VALUE_INIT((class_), (flags_))
#define QSPINLOCK_INIT_CHK(class_, flags_)                                     \
    ((struct qspinlock) {                                                      \
        .val = ATOMIC_VAR_INIT(0) __QSPINLOCK_LOCK_CHK_VALUE_INIT(             \
            (class_), (flags_)) __QSPINLOCK_SHALLOW_VALUE_INIT})

#define qspinlock_init_chk(lock_, class_, flags_)                              \
    qspinlock_init_chk_full((lock_), (class_), (flags_))
#define qspinlock_init_auto_internal(lock_, flags_)                            \
    do {                                                                       \
        static const struct lock_chk_class __auto_class = {                    \
            .name = #lock_,                                                    \
            .file = __RELFILE__,                                               \
            .line = __LINE__,                                                  \
        };                                                                     \
        qspinlock_init_chk_full((lock_), &__auto_class, (flags_));             \
    } while (0)

static inline void qspinlock_policy_init_internal(struct qspinlock *lock,
                                                  enum lock_chk_flags flags) {
    enforce((flags & ~LOCK_CHKD_FULL) == 0);
    lock->chk.flags = flags;
    lock->chk.initialized = true;
    atomic_store_rlx(&lock->chk.used, false);
}

static inline void
qspinlock_map_init_internal(struct qspinlock *lock,
                            const struct lock_chk_class *class,
                            enum lock_chk_flags flags) {
    enforce(flags == LOCK_UNCHKD || class != NULL);
    lock_chk_map_runtime_init(&lock->chk.map, class);
}

static inline void qspinlock_shallow_init_internal(struct qspinlock *lock) {
    atomic_store_rlx(&lock->irq_usage, LOCK_OP_IRQ_NONE);
}

static inline void qspinlock_set_chk_flags(struct qspinlock *lock,
                                           enum lock_chk_flags flags) {
    enforce(lock->chk.initialized);
    enforce(!qspin_is_locked(lock));
    enforce(!atomic_load_rlx(&lock->chk.used));
    enforce((flags & ~LOCK_CHKD_FULL) == 0);
    lock->chk.flags = flags;
}

static inline void qspinlock_reinit_chk(struct qspinlock *lock,
                                        const struct lock_chk_class *class,
                                        enum lock_chk_flags flags) {
    enforce(lock->chk.initialized);
    enforce(!qspin_is_locked(lock));
    qspinlock_init_chk_full(lock, class, flags);
}

static inline void qspinlock_note_use(struct qspinlock *lock,
                                      enum lock_op_flags flags) {
    lock_chk_note_use(&lock->chk, flags);
}

static inline bool qspinlock_order_checked(struct qspinlock *lock) {
    return lock_chk_tracking_active() &&
           (lock->chk.flags & LOCK_CHKD_ORDER) != 0;
}

/* TODO: move stamping to initialization, but
 * we are using this HACK:
 * because it's fine anyways */
static inline void qspinlock_stamp(struct qspinlock *lock) {
    lock->chk.instance = lock;
    lock->chk.type = LOCK_CHK_TYPE_QSPIN;
}

static inline void qspinlock_classify(struct qspinlock *lock,
                                      enum lock_op_flags usage,
                                      const struct lock_chk_site *site) {
    qspinlock_stamp(lock);
    lock_debug_spin_classify(&lock->irq_usage, usage, &lock->chk, site);
}

static inline bool qspinlock_deep_checked(struct qspinlock *lock) {
    return lock_chk_tracking_active() && lock->chk.flags != LOCK_UNCHKD;
}

static inline struct lock_chk_acq_req
qspinlock_chk_acq_req(struct qspinlock *lock, const struct lock_chk_site *site,
                      uint8_t subclass, enum lock_op_flags flags) {
    qspinlock_stamp(lock);
    return lock_chk_acq_req_make(&lock->chk, site, LOCK_CHK_MODE_EXCLUSIVE,
                                 subclass, flags);
}

static inline struct lock_chk_rel_req
qspinlock_chk_rel_req(struct qspinlock *lock,
                      const struct lock_chk_site *site) {
    qspinlock_stamp(lock);
    return lock_chk_rel_req_make(&lock->chk, site, LOCK_CHK_MODE_EXCLUSIVE);
}

struct qspinlock_acq_scope {
    struct lock_chk_acq_req req;
    struct lock_chk_acq_token token;
    bool checked;
};

struct qspinlock_rel_scope {
    struct lock_chk_rel_req req;
    struct lock_chk_rel_token token;
    bool checked;
};

static inline void qspinlock_acq_begin(struct qspinlock_acq_scope *scope,
                                       struct qspinlock *lock,
                                       const struct lock_chk_site *site,
                                       uint8_t subclass,
                                       enum lock_op_flags flags) {
    scope->checked = qspinlock_deep_checked(lock);
    if (!scope->checked)
        return;

    scope->req = qspinlock_chk_acq_req(lock, site, subclass, flags);
    lock_chk_before_acq(&scope->token, &scope->req);
}

static inline void qspinlock_acq_commit(struct qspinlock_acq_scope *scope) {
    if (scope->checked)
        lock_chk_acqd(&scope->token);
}

static inline void qspinlock_acq_abort(struct qspinlock_acq_scope *scope) {
    if (scope->checked)
        lock_chk_cancel(&scope->token);
}

static inline void qspinlock_rel_begin(struct qspinlock_rel_scope *scope,
                                       struct qspinlock *lock,
                                       const struct lock_chk_site *site) {
    scope->checked = qspinlock_deep_checked(lock);
    if (!scope->checked)
        return;

    scope->req = qspinlock_chk_rel_req(lock, site);
    lock_chk_before_rel(&scope->token, &scope->req);
}

static inline void qspinlock_rel_commit(struct qspinlock_rel_scope *scope) {
    if (scope->checked)
        lock_chk_reld(&scope->token);
}

static inline void qspinlock_shallow_push(struct qspinlock *lock,
                                          enum irql irql,
                                          const struct lock_chk_site *site) {
    qspinlock_stamp(lock);
    lock_debug_spin_push(&lock->chk, irql, site);
}

static inline void
qspinlock_shallow_validate_top(struct qspinlock *lock, enum irql old,
                               const struct lock_chk_site *site) {
    qspinlock_stamp(lock);
    lock_debug_spin_validate_top(&lock->chk, old, site);
}

static inline void qspinlock_shallow_pop(struct qspinlock *lock) {
    lock_debug_spin_pop(&lock->chk);
}

static inline bool
qspinlock_assert_held_deep(struct qspinlock *lock, bool want_held,
                           const struct lock_chk_site *site) {
    qspinlock_stamp(lock);
    return qspinlock_deep_checked(lock) &&
           lock_chk_assert_held_deep(&lock->chk, LOCK_CHK_MODE_IGNORED,
                                     want_held, site);
}

#else /* !defined(DEBUG_LOCK_CHK) */

#define __QSPINLOCK_SHALLOW_VALUE_INIT
#define __QSPINLOCK_LOCK_CHK_VALUE_INIT(class_, flags_)
#define QSPINLOCK_INIT_CHK(class_, flags_)                                     \
    ((struct qspinlock) {.val = ATOMIC_VAR_INIT(0)})

#define qspinlock_init_chk(lock_, class_, flags_)                              \
    qspinlock_init_chk_full((lock_), NULL, LOCK_UNCHKD)
#define qspinlock_init_auto_internal(lock_, flags_)                            \
    qspinlock_init_chk_full((lock_), NULL, LOCK_UNCHKD)

static inline void qspinlock_policy_init_internal(struct qspinlock *lock,
                                                  enum lock_chk_flags flags) {
    cc_unused(lock, flags);
}

static inline void
qspinlock_map_init_internal(struct qspinlock *lock,
                            const struct lock_chk_class *class,
                            enum lock_chk_flags flags) {
    cc_unused(lock, class, flags);
}

static inline void qspinlock_shallow_init_internal(struct qspinlock *lock) {
    cc_unused(lock);
}

static inline void qspinlock_set_chk_flags(struct qspinlock *lock,
                                           enum lock_chk_flags flags) {
    cc_unused(lock, flags);
}

static inline void qspinlock_reinit_chk(struct qspinlock *lock,
                                        const struct lock_chk_class *class,
                                        enum lock_chk_flags flags) {
    cc_unused(lock, class, flags);
}

static inline void qspinlock_note_use(struct qspinlock *lock,
                                      enum lock_op_flags flags) {
    cc_unused(lock, flags);
}

static inline bool qspinlock_order_checked(struct qspinlock *lock) {
    cc_unused(lock);
    return false;
}

static inline void qspinlock_classify(struct qspinlock *lock,
                                      enum lock_op_flags usage,
                                      const struct lock_chk_site *site) {
    cc_unused(lock, usage, site);
}

static inline bool qspinlock_deep_checked(struct qspinlock *lock) {
    cc_unused(lock);
    return false;
}

struct qspinlock_acq_scope {
    char unused_;
};

struct qspinlock_rel_scope {
    char unused_;
};

static inline void qspinlock_acq_begin(struct qspinlock_acq_scope *scope,
                                       struct qspinlock *lock,
                                       const struct lock_chk_site *site,
                                       uint8_t subclass,
                                       enum lock_op_flags flags) {
    cc_unused(scope, lock, site, subclass, flags);
}

static inline void qspinlock_acq_commit(struct qspinlock_acq_scope *scope) {
    cc_unused(scope);
}

static inline void qspinlock_acq_abort(struct qspinlock_acq_scope *scope) {
    cc_unused(scope);
}

static inline void qspinlock_rel_begin(struct qspinlock_rel_scope *scope,
                                       struct qspinlock *lock,
                                       const struct lock_chk_site *site) {
    cc_unused(scope, lock, site);
}

static inline void qspinlock_rel_commit(struct qspinlock_rel_scope *scope) {
    cc_unused(scope);
}

static inline void qspinlock_shallow_push(struct qspinlock *lock,
                                          enum irql irql,
                                          const struct lock_chk_site *site) {
    cc_unused(lock, irql, site);
}

static inline void
qspinlock_shallow_validate_top(struct qspinlock *lock, enum irql old,
                               const struct lock_chk_site *site) {
    cc_unused(lock, old, site);
}

static inline void qspinlock_shallow_pop(struct qspinlock *lock) {
    cc_unused(lock);
}

static inline bool
qspinlock_assert_held_deep(struct qspinlock *lock, bool want_held,
                           const struct lock_chk_site *site) {
    cc_unused(lock, want_held, site);
    return false;
}

#endif /* DEBUG_LOCK_CHK */

static inline void qspinlock_init_chk_full(struct qspinlock *lock,
                                           const struct lock_chk_class *class,
                                           enum lock_chk_flags flags) {
    atomic_store_rlx(&lock->val, 0);
    qspinlock_policy_init_internal(lock, flags);
    qspinlock_shallow_init_internal(lock);
    qspinlock_map_init_internal(lock, class, flags);
}

#define qspinlock_init_1(lock_) qspinlock_init_2(lock_, LOCK_CHKD_FULL)
#define qspinlock_init_2(lock_, flags_)                                        \
    qspinlock_init_auto_internal((lock_), (flags_))
#define qspinlock_init(...) PP_CALL(qspinlock_init, __VA_ARGS__)
#define qspin_init(...) qspinlock_init(__VA_ARGS__)
#define qspin_init_chk(...) qspinlock_init_chk(__VA_ARGS__)

static inline cc_warn_unused_result bool
qspin_trylock_physical(struct qspinlock *lock) TSA_NO_ANALYSIS {
    uint32_t expected = 0;
    return atomic_cas_strong(&lock->val, &expected, Q_SPIN_LOCKED_VAL,
                             mo_acquire, mo_rlx);
}

void qspin_lock_slowpath(struct qspinlock *lock, uint32_t val);

static inline void qspin_lock_physical(struct qspinlock *lock) TSA_NO_ANALYSIS {
    uint32_t val = 0;
    if (cc_likely(atomic_cas_strong(&lock->val, &val, Q_SPIN_LOCKED_VAL,
                                    mo_acquire, mo_rlx)))
        return;

    qspin_lock_slowpath(lock, val);
}

static inline void
qspin_unlock_physical(struct qspinlock *lock) TSA_NO_ANALYSIS {
    /* Clear only the locked byte so pending and tail bits remain valid */
    atomic_store_release((atomic_uint8_t *) &lock->val, 0);
}

static inline bool qspin_is_locked(const struct qspinlock *lock) {
    return (atomic_load_rlx(&lock->val) & Q_SPIN_LOCKED_MASK) != 0;
}

/* Same deal as SPINLOCK_ASSERT_LOCKED, just checks the bit,
 * but that doesn't say anything about thread ownership */
#define QSPINLOCK_ASSERT_LOCKED(l)                                             \
    enforce(qspin_is_locked(l), "qspinlock not locked")

static inline cc_warn_unused_result bool
qspin_trylock_raw_full(struct qspinlock *lock, const struct lock_chk_site *site)
    TSA_TRY_ACQUIRES(true, lock) TSA_NO_ANALYSIS {
    enum lock_op_flags flags =
        LOCK_OP_RAW | LOCK_OP_IRQ_NONE | LOCK_OP_KIND_TRY;
    qspinlock_note_use(lock, flags);

    struct qspinlock_acq_scope chk;
    qspinlock_acq_begin(&chk, lock, site, 0, flags);

    if (qspin_trylock_physical(lock)) {

        qspinlock_acq_commit(&chk);

        return true;
    }

    qspinlock_acq_abort(&chk);

    return false;
}

static inline void qspin_lock_raw_full(struct qspinlock *lock,
                                       const struct lock_chk_site *site)
    TSA_ACQUIRES(lock) TSA_NO_ANALYSIS {
    enum lock_op_flags flags =
        LOCK_OP_RAW | LOCK_OP_IRQ_NONE | LOCK_OP_KIND_BLOCKING;
    qspinlock_note_use(lock, flags);

    struct qspinlock_acq_scope chk;
    qspinlock_acq_begin(&chk, lock, site, 0, flags);

    qspin_lock_physical(lock);

    qspinlock_acq_commit(&chk);
}

static inline void qspin_unlock_raw_full(struct qspinlock *lock,
                                         const struct lock_chk_site *site)
    TSA_RELEASES(lock) TSA_NO_ANALYSIS {

    struct qspinlock_rel_scope chk;
    qspinlock_rel_begin(&chk, lock, site);

    qspin_unlock_physical(lock);

    qspinlock_rel_commit(&chk);
}

static inline void qspin_unlock_full(struct qspinlock *lock, enum irql old_irql,
                                     const struct lock_chk_site *site)
    TSA_RELEASES_SPIN(lock) TSA_NO_ANALYSIS {
    bool checked_shallow = qspinlock_order_checked(lock);
    struct qspinlock_rel_scope chk;

    bool irqs_enabled = irq_disable_save();

    if (checked_shallow)
        qspinlock_shallow_validate_top(lock, old_irql, site);
    qspinlock_rel_begin(&chk, lock, site);

    qspin_unlock_physical(lock);

    qspinlock_rel_commit(&chk);
    if (checked_shallow)
        qspinlock_shallow_pop(lock);

    crash_unwind_exit_qspinlock(lock);
    if (irqs_enabled)
        irq_enable();

    irql_lower(old_irql);
}

static inline cc_warn_unused_result enum irql
qspin_lock_subclass_full(struct qspinlock *lock, uint8_t subclass,
                         const struct lock_chk_site *site)
    TSA_ACQUIRES_SPIN(lock) TSA_NO_ANALYSIS {
    enforce(subclass < LOCK_CHK_MAX_SUBCLASSES);
    if (bootstage_get() >= BOOTSTAGE_MID_MP &&
        (irq_in_interrupt() || irq_in_nmi()))
        panic(
            "Attempted to take non-ISR safe qspinlock outside thread context");

    enum lock_op_flags flags = LOCK_OP_IRQ_DISPATCH | LOCK_OP_KIND_BLOCKING;
    qspinlock_note_use(lock, flags);
    bool checked_shallow = qspinlock_order_checked(lock);
    if (checked_shallow)
        qspinlock_classify(lock, flags, site);

    struct qspinlock_acq_scope chk;
    qspinlock_acq_begin(&chk, lock, site, subclass, flags);

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    qspin_lock_physical(lock);

    enforce(irqs_enabled());
    irq_disable();

    if (checked_shallow)
        qspinlock_shallow_push(lock, irql, site);
    qspinlock_acq_commit(&chk);

    crash_unwind_enter_qspinlock(lock, irql);
    irq_enable();

    return irql;
}

static inline cc_warn_unused_result enum irql
qspin_lock_full(struct qspinlock *lock, const struct lock_chk_site *site)
    TSA_ACQUIRES_SPIN(lock) TSA_NO_ANALYSIS {
    return qspin_lock_subclass_full(lock, 0, site);
}

static inline cc_warn_unused_result enum irql
qspin_lock_high_full(struct qspinlock *lock, const struct lock_chk_site *site)
    TSA_ACQUIRES_SPIN(lock) TSA_NO_ANALYSIS {
    if (bootstage_get() >= BOOTSTAGE_MID_MP && irq_in_nmi())
        panic("Attempted to take non-raw qspinlock from an NMI");

    enum lock_op_flags flags = LOCK_OP_KIND_BLOCKING | LOCK_OP_IRQ_HIGH;
    qspinlock_note_use(lock, flags);
    bool checked_shallow = qspinlock_order_checked(lock);
    if (checked_shallow)
        qspinlock_classify(lock, flags, site);

    struct qspinlock_acq_scope chk;
    qspinlock_acq_begin(&chk, lock, site, 0, flags);

    enum irql irql = irql_raise(IRQL_HIGH_LEVEL);
    qspin_lock_physical(lock);

    if (checked_shallow)
        qspinlock_shallow_push(lock, irql, site);
    qspinlock_acq_commit(&chk);

    crash_unwind_enter_qspinlock(lock, irql);

    return irql;
}

static inline cc_warn_unused_result bool
qspin_trylock_full(struct qspinlock *lock, enum irql *out,
                   const struct lock_chk_site *site)
    TSA_TRY_ACQUIRES_SPIN(true, lock) TSA_NO_ANALYSIS {
    if (bootstage_get() >= BOOTSTAGE_MID_MP &&
        (irq_in_interrupt() || irq_in_nmi()))
        panic(
            "Attempted to take non-ISR safe qspinlock outside thread context");

    enum lock_op_flags flags = LOCK_OP_KIND_TRY | LOCK_OP_IRQ_DISPATCH;
    qspinlock_note_use(lock, flags);
    bool checked_shallow = qspinlock_order_checked(lock);
    if (checked_shallow)
        qspinlock_classify(lock, flags, site);

    struct qspinlock_acq_scope chk;
    qspinlock_acq_begin(&chk, lock, site, 0, flags);

    *out = irql_raise(IRQL_DISPATCH_LEVEL);
    if (qspin_trylock_physical(lock)) {
        enforce(irqs_enabled()); /* Should not go false */
        irq_disable();
        if (checked_shallow)
            qspinlock_shallow_push(lock, *out, site);
        qspinlock_acq_commit(&chk);
        irq_enable();
        crash_unwind_enter_qspinlock(lock, *out);
        return true;
    }

    qspinlock_acq_abort(&chk);

    irql_lower(*out);
    return false;
}

static inline cc_warn_unused_result bool
qspin_trylock_high_full(struct qspinlock *lock, enum irql *out,
                        const struct lock_chk_site *site)
    TSA_TRY_ACQUIRES_SPIN(true, lock) TSA_NO_ANALYSIS {
    if (bootstage_get() >= BOOTSTAGE_MID_MP && irq_in_nmi())
        panic("Attempted to take non-raw qspinlock from an NMI");

    enum lock_op_flags flags = LOCK_OP_KIND_BLOCKING | LOCK_OP_IRQ_HIGH;
    qspinlock_note_use(lock, flags);
    bool checked_shallow = qspinlock_order_checked(lock);
    if (checked_shallow)
        qspinlock_classify(lock, flags, site);

    struct qspinlock_acq_scope chk;
    qspinlock_acq_begin(&chk, lock, site, 0, flags);

    *out = irql_raise(IRQL_HIGH_LEVEL);
    if (qspin_trylock_physical(lock)) {

        if (checked_shallow)
            qspinlock_shallow_push(lock, *out, site);
        qspinlock_acq_commit(&chk);

        crash_unwind_enter_qspinlock(lock, *out);
        return true;
    }

    qspinlock_acq_abort(&chk);

    irql_lower(*out);
    return false;
}

#define qspin_lock(lock_) qspin_lock_full((lock_), LOCK_CHK_SITE_HERE())
#define qspin_lock_subclass(lock_, subclass_)                                  \
    qspin_lock_subclass_full((lock_), (subclass_), LOCK_CHK_SITE_HERE())
#define qspin_lock_high(lock_)                                                 \
    qspin_lock_high_full((lock_), LOCK_CHK_SITE_HERE())
#define qspin_unlock(lock_, old_)                                              \
    qspin_unlock_full((lock_), (old_), LOCK_CHK_SITE_HERE())
#define qspin_unlock_irq_restore(lock_, old_)                                  \
    qspin_unlock_full((lock_), (old_), LOCK_CHK_SITE_HERE())
#define qspin_trylock(lock_, out_)                                             \
    qspin_trylock_full((lock_), (out_), LOCK_CHK_SITE_HERE())
#define qspin_trylock_high(lock_, out_)                                        \
    qspin_trylock_high_full((lock_), (out_), LOCK_CHK_SITE_HERE())
#define qspin_lock_raw(lock_) qspin_lock_raw_full((lock_), LOCK_CHK_SITE_HERE())
#define qspin_trylock_raw(lock_)                                               \
    qspin_trylock_raw_full((lock_), LOCK_CHK_SITE_HERE())
#define qspin_unlock_raw(lock_)                                                \
    qspin_unlock_raw_full((lock_), LOCK_CHK_SITE_HERE())

static inline void qspin_assert_held_full(struct qspinlock *lock,
                                          const struct lock_chk_site *site)
    TSA_ASSERT_CAPABILITY(lock) {
    if (qspinlock_assert_held_deep(lock, /*want_held=*/true, site))
        return;

    QSPINLOCK_ASSERT_LOCKED(lock);
}

static inline void
qspin_assert_not_held_full(struct qspinlock *lock,
                           const struct lock_chk_site *site) {
    if (qspinlock_assert_held_deep(lock, /*want_held=*/false, site))
        return;

    if (bootstage_get() < BOOTSTAGE_EARLY_DEVICES)
        enforce(!qspin_is_locked(lock), "qspinlock unexpectedly locked");
}

#define QSPINLOCK_ASSERT_HELD(l)                                               \
    qspin_assert_held_full((l), LOCK_CHK_SITE_HERE())
#define QSPINLOCK_ASSERT_NOT_HELD(l)                                           \
    qspin_assert_not_held_full((l), LOCK_CHK_SITE_HERE())

struct qspinlock_guard {
    struct qspinlock *lock;
    enum irql irql;
};

static inline cc_always_inline cc_maybe_unused void
qspinlock_guard_assume(struct qspinlock *lock) TSA_ASSERT_CAPABILITY(lock) {
    cc_unused(lock);
}

static inline cc_always_inline cc_maybe_unused void qspinlock_guard_exit(
    struct qspinlock_guard *g) TSA_RELEASES_SPIN_IRQL TSA_NO_ANALYSIS {
    if (g->lock)
        qspin_unlock(g->lock, g->irql);
}

static inline cc_always_inline cc_maybe_unused struct qspinlock_guard
qspin_guard_enter(struct qspinlock *lock)
    TSA_ACQUIRES_SPIN_IRQL TSA_NO_ANALYSIS {
    return (struct qspinlock_guard) {
        .lock = lock,
        .irql = qspin_lock(lock),
    };
}

static inline cc_always_inline cc_maybe_unused struct qspinlock_guard
qspin_guard_high_enter(struct qspinlock *lock)
    TSA_ACQUIRES_SPIN_IRQL TSA_NO_ANALYSIS {
    return (struct qspinlock_guard) {
        .lock = lock,
        .irql = qspin_lock_high(lock),
    };
}

static inline cc_always_inline cc_maybe_unused struct qspinlock_guard
qspin_guard_subclass_enter(struct qspinlock *lock, uint8_t subclass)
    TSA_ACQUIRES_SPIN_IRQL TSA_NO_ANALYSIS {
    return (struct qspinlock_guard) {
        .lock = lock,
        .irql = qspin_lock_subclass(lock, subclass),
    };
}

#define qspin_guard(lock_)                                                     \
    cc_cleanup(qspinlock_guard_exit) struct qspinlock_guard PP_CONCAT(         \
        qspin_guard_, __COUNTER__) =                                           \
        (qspinlock_guard_assume(lock_), qspin_guard_enter(lock_))

#define qspin_guard_high(lock_)                                                \
    cc_cleanup(qspinlock_guard_exit) struct qspinlock_guard PP_CONCAT(         \
        qspin_guard_high_, __COUNTER__) =                                      \
        (qspinlock_guard_assume(lock_), qspin_guard_high_enter(lock_))

#define qspin_guard_subclass(lock_, subclass_)                                 \
    cc_cleanup(qspinlock_guard_exit) struct qspinlock_guard PP_CONCAT(         \
        qspin_guard_, __COUNTER__) =                                           \
        (qspinlock_guard_assume(lock_),                                        \
         qspin_guard_subclass_enter((lock_), (subclass_)))
