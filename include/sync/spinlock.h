#pragma once
#include <asm.h>
#include <atomic.h>
#include <bootstage.h>
#include <compiler/core.h>
#include <console/panic.h>
#include <enforce.h>
#include <irq/irq.h>
#include <sch/irql.h>
#include <smp/core.h>
#include <stdbool.h>
#include <sync/lock_chk_types.h>
#include <sync/raw_spinlock.h>
#include <tsa.h>

struct TSA_CAPABILITY("spinlock") spinlock {
    struct raw_spinlock raw;

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_lock chk;
    atomic(enum lock_op_flags) irq_usage;
#endif /* DEBUG_LOCK_CHK */
};

TSA_DEFINE_ASSUME(struct spinlock)

#define SPINLOCK_INIT SPINLOCK_INIT_CHK(NULL, LOCK_CHKD_FULL)
#define SPINLOCK_DEFINE(id) struct spinlock id = SPINLOCK_INIT
#define SPINLOCK_DEFINE_CHK(id, class_, flags_)                                \
    struct spinlock id = SPINLOCK_INIT_CHK((class_), (flags_))

#define spinlock_init_1(lock_) spinlock_init_2(lock_, LOCK_CHKD_FULL)
#define spinlock_init_2(lock_, flags_)                                         \
    spinlock_init_auto_internal((lock_), (flags_))
#define spinlock_init(...) PP_CALL(spinlock_init, __VA_ARGS__)

static inline void spinlock_init_chk_full(struct spinlock *lock,
                                          const struct lock_chk_class *class,
                                          enum lock_chk_flags flags);

#ifdef DEBUG_LOCK_CHK

#define __SPINLOCK_SHALLOW_VALUE_INIT                                          \
    , .irq_usage = ATOMIC_VAR_INIT(LOCK_OP_IRQ_NONE)
#define __SPINLOCK_LOCK_CHK_VALUE_INIT(class_, flags_)                         \
    , .chk = LOCK_CHK_LOCK_VALUE_INIT((class_), (flags_))
#define SPINLOCK_INIT_CHK(class_, flags_)                                      \
    ((struct spinlock) {                                                       \
        .raw = RAW_SPINLOCK_INIT __SPINLOCK_LOCK_CHK_VALUE_INIT(               \
            (class_), (flags_)) __SPINLOCK_SHALLOW_VALUE_INIT})

#define spinlock_init_chk(lock_, class_, flags_)                               \
    spinlock_init_chk_full((lock_), (class_), (flags_))
#define spinlock_init_auto_internal(lock_, flags_)                             \
    do {                                                                       \
        static const struct lock_chk_class __auto_class = {                    \
            .name = #lock_,                                                    \
            .file = __RELFILE__,                                               \
            .line = __LINE__,                                                  \
        };                                                                     \
        spinlock_init_chk_full((lock_), &__auto_class, (flags_));              \
    } while (0)

static inline void spinlock_policy_init_internal(struct spinlock *lock,
                                                 enum lock_chk_flags flags) {
    enforce((flags & ~LOCK_CHKD_FULL) == 0);
    lock->chk.flags = flags;
    lock->chk.initialized = true;
    atomic_store_rlx(&lock->chk.used, false);
}
static inline void spinlock_shallow_init_internal(struct spinlock *lock) {
    atomic_store_rlx(&lock->irq_usage, LOCK_OP_IRQ_NONE);
}
static inline void
spinlock_map_init_internal(struct spinlock *lock,
                           const struct lock_chk_class *class,
                           enum lock_chk_flags flags) {
    enforce(flags == LOCK_UNCHKD || class != NULL);
    lock_chk_map_runtime_init(&lock->chk.map, class);
}

static inline void spinlock_note_use(struct spinlock *lock,
                                     enum lock_op_flags flags) {
    lock_chk_note_use(&lock->chk, flags);
}

static inline bool spinlock_order_checked(struct spinlock *lock) {
    return lock_chk_tracking_active() &&
           (lock->chk.flags & LOCK_CHKD_ORDER) != 0;
}

/* TODO: unify stamping at initialization too */
static inline void spinlock_stamp(struct spinlock *lock) {
    lock->chk.instance = lock;
    lock->chk.type = LOCK_CHK_TYPE_SPIN;
}

static inline void spinlock_classify(struct spinlock *lock,
                                     enum lock_op_flags usage,
                                     const struct lock_chk_site *site) {
    spinlock_stamp(lock);
    lock_debug_spin_classify(&lock->irq_usage, usage, &lock->chk, site);
}

static inline bool spinlock_deep_checked(struct spinlock *lock) {
    return lock_chk_tracking_active() && lock->chk.flags != LOCK_UNCHKD;
}

static inline struct lock_chk_acq_req
spinlock_chk_acq_req(struct spinlock *lock, const struct lock_chk_site *site,
                     uint8_t subclass, enum lock_op_flags flags) {
    spinlock_stamp(lock);
    return lock_chk_acq_req_make(&lock->chk, site, LOCK_CHK_MODE_EXCLUSIVE,
                                 subclass, flags);
}

static inline struct lock_chk_rel_req
spinlock_chk_rel_req(struct spinlock *lock, const struct lock_chk_site *site) {
    spinlock_stamp(lock);
    return lock_chk_rel_req_make(&lock->chk, site, LOCK_CHK_MODE_EXCLUSIVE);
}

struct spinlock_acq_scope {
    struct lock_chk_acq_req req;
    struct lock_chk_acq_token token;
    bool checked;
};

struct spinlock_rel_scope {
    struct lock_chk_rel_req req;
    struct lock_chk_rel_token token;
    bool checked;
};

static inline void spinlock_acq_begin(struct spinlock_acq_scope *scope,
                                      struct spinlock *lock,
                                      const struct lock_chk_site *site,
                                      uint8_t subclass,
                                      enum lock_op_flags flags) {
    scope->checked = spinlock_deep_checked(lock);
    if (!scope->checked)
        return;

    scope->req = spinlock_chk_acq_req(lock, site, subclass, flags);
    lock_chk_before_acq(&scope->token, &scope->req);
}

static inline void spinlock_acq_commit(struct spinlock_acq_scope *scope) {
    if (scope->checked)
        lock_chk_acqd(&scope->token);
}

static inline void spinlock_acq_abort(struct spinlock_acq_scope *scope) {
    if (scope->checked)
        lock_chk_cancel(&scope->token);
}

static inline void spinlock_rel_begin(struct spinlock_rel_scope *scope,
                                      struct spinlock *lock,
                                      const struct lock_chk_site *site) {
    scope->checked = spinlock_deep_checked(lock);
    if (!scope->checked)
        return;

    scope->req = spinlock_chk_rel_req(lock, site);
    lock_chk_before_rel(&scope->token, &scope->req);
}

static inline void spinlock_rel_commit(struct spinlock_rel_scope *scope) {
    if (scope->checked)
        lock_chk_reld(&scope->token);
}

static inline void spinlock_shallow_push(struct spinlock *lock, enum irql irql,
                                         const struct lock_chk_site *site) {
    spinlock_stamp(lock);
    lock_debug_spin_push(&lock->chk, irql, site);
}

static inline void
spinlock_shallow_validate_top(struct spinlock *lock, enum irql old,
                              const struct lock_chk_site *site) {
    spinlock_stamp(lock);
    lock_debug_spin_validate_top(&lock->chk, old, site);
}

static inline void spinlock_shallow_pop(struct spinlock *lock) {
    lock_debug_spin_pop(&lock->chk);
}

static inline bool spinlock_assert_held_deep(struct spinlock *lock,
                                             bool want_held,
                                             const struct lock_chk_site *site) {
    spinlock_stamp(lock);
    return spinlock_deep_checked(lock) &&
           lock_chk_assert_held_deep(&lock->chk, LOCK_CHK_MODE_IGNORED,
                                     want_held, site);
}

static inline void spinlock_assert_initialized(struct spinlock *lock) {
    enforce(lock->chk.initialized);
}

static inline void spinlock_policy_set_internal(struct spinlock *lock,
                                                enum lock_chk_flags flags) {
    enforce(lock->chk.initialized);
    enforce(!atomic_load_rlx(&lock->chk.used));
    enforce((flags & ~LOCK_CHKD_FULL) == 0);
    lock->chk.flags = flags;
}

#else /* !defined(DEBUG_LOCK_CHK) */

#define __SPINLOCK_SHALLOW_VALUE_INIT
#define __SPINLOCK_LOCK_CHK_VALUE_INIT(class_, flags_)
#define SPINLOCK_INIT_CHK(class_, flags_)                                      \
    ((struct spinlock) {.raw = RAW_SPINLOCK_INIT})

#define spinlock_init_chk(lock_, class_, flags_)                               \
    spinlock_init_chk_full((lock_), NULL, LOCK_UNCHKD)
#define spinlock_init_auto_internal(lock_, flags_)                             \
    spinlock_init_chk_full((lock_), NULL, LOCK_UNCHKD)

static inline void spinlock_policy_init_internal(struct spinlock *lock,
                                                 enum lock_chk_flags flags) {
    cc_unused(lock, flags);
}

static inline void spinlock_shallow_init_internal(struct spinlock *lock) {
    cc_unused(lock);
}

static inline void
spinlock_map_init_internal(struct spinlock *lock,
                           const struct lock_chk_class *class,
                           enum lock_chk_flags flags) {
    cc_unused(lock, class, flags);
}

static inline void spinlock_note_use(struct spinlock *lock,
                                     enum lock_op_flags flags) {
    cc_unused(lock, flags);
}

static inline bool spinlock_order_checked(struct spinlock *lock) {
    cc_unused(lock);
    return false;
}

static inline void spinlock_classify(struct spinlock *lock,
                                     enum lock_op_flags usage,
                                     const struct lock_chk_site *site) {
    cc_unused(lock, usage, site);
}

static inline bool spinlock_deep_checked(struct spinlock *lock) {
    cc_unused(lock);
    return false;
}

struct spinlock_acq_scope {
    char unused_;
};

struct spinlock_rel_scope {
    char unused_;
};

static inline void spinlock_acq_begin(struct spinlock_acq_scope *scope,
                                      struct spinlock *lock,
                                      const struct lock_chk_site *site,
                                      uint8_t subclass,
                                      enum lock_op_flags flags) {
    cc_unused(scope, lock, site, subclass, flags);
}

static inline void spinlock_acq_commit(struct spinlock_acq_scope *scope) {
    cc_unused(scope);
}

static inline void spinlock_acq_abort(struct spinlock_acq_scope *scope) {
    cc_unused(scope);
}

static inline void spinlock_rel_begin(struct spinlock_rel_scope *scope,
                                      struct spinlock *lock,
                                      const struct lock_chk_site *site) {
    cc_unused(scope, lock, site);
}

static inline void spinlock_rel_commit(struct spinlock_rel_scope *scope) {
    cc_unused(scope);
}

static inline void spinlock_shallow_push(struct spinlock *lock, enum irql irql,
                                         const struct lock_chk_site *site) {
    cc_unused(lock, irql, site);
}

static inline void
spinlock_shallow_validate_top(struct spinlock *lock, enum irql old,
                              const struct lock_chk_site *site) {
    cc_unused(lock, old, site);
}

static inline void spinlock_shallow_pop(struct spinlock *lock) {
    cc_unused(lock);
}

static inline bool spinlock_assert_held_deep(struct spinlock *lock,
                                             bool want_held,
                                             const struct lock_chk_site *site) {
    cc_unused(lock, want_held, site);
    return false;
}

static inline void spinlock_assert_initialized(struct spinlock *lock) {
    cc_unused(lock);
}

static inline void spinlock_policy_set_internal(struct spinlock *lock,
                                                enum lock_chk_flags flags) {
    cc_unused(lock, flags);
}

#endif /* DEBUG_LOCK_CHK */

static inline cc_warn_unused_result bool
spin_trylock_physical(struct spinlock *lock) TSA_NO_ANALYSIS {
    return raw_spin_trylock(&lock->raw);
}

static inline void spin_lock_physical(struct spinlock *lock) TSA_NO_ANALYSIS {
    raw_spin_lock(&lock->raw);
}

static inline void spin_unlock_physical(struct spinlock *lock) TSA_NO_ANALYSIS {
    raw_spin_unlock(&lock->raw);
}

static inline void spinlock_init_chk_full(struct spinlock *lock,
                                          const struct lock_chk_class *class,
                                          enum lock_chk_flags flags) {
    raw_spinlock_init(&lock->raw);
    spinlock_policy_init_internal(lock, flags);
    spinlock_shallow_init_internal(lock);
    spinlock_map_init_internal(lock, class, flags);
}

static inline void spinlock_restore_interrupts(bool enabled) TSA_RELEASES_IRQS {
    irq_restore(enabled);
}

static inline cc_warn_unused_result bool
spin_trylock_raw_full(struct spinlock *lock, const struct lock_chk_site *site)
    TSA_TRY_ACQUIRES(true, lock) TSA_NO_ANALYSIS {
    enum lock_op_flags flags =
        LOCK_OP_RAW | LOCK_OP_KIND_TRY | LOCK_OP_IRQ_NONE;
    spinlock_note_use(lock, flags);

    struct spinlock_acq_scope chk;
    spinlock_acq_begin(&chk, lock, site, 0, flags);

    if (spin_trylock_physical(lock)) {
        spinlock_acq_commit(&chk);
        return true;
    }

    spinlock_acq_abort(&chk);
    return false;
}

static inline void spin_lock_raw_full(struct spinlock *lock,
                                      const struct lock_chk_site *site)
    TSA_ACQUIRES(lock) TSA_NO_ANALYSIS {
    enum lock_op_flags flags =
        LOCK_OP_RAW | LOCK_OP_KIND_BLOCKING | LOCK_OP_IRQ_NONE;
    spinlock_note_use(lock, flags);

    struct spinlock_acq_scope chk;
    spinlock_acq_begin(&chk, lock, site, 0, flags);

    spin_lock_physical(lock);
    spinlock_acq_commit(&chk);
}

static inline void spin_unlock_raw_full(struct spinlock *lock,
                                        const struct lock_chk_site *site)
    TSA_RELEASES(lock) TSA_NO_ANALYSIS {

    struct spinlock_rel_scope chk;
    spinlock_rel_begin(&chk, lock, site);

    spin_unlock_physical(lock);
    spinlock_rel_commit(&chk);
}

static inline void spin_unlock_full(struct spinlock *lock, enum irql old,
                                    const struct lock_chk_site *site)
    TSA_RELEASES_SPIN(lock) TSA_NO_ANALYSIS {

    bool checked_shallow = spinlock_order_checked(lock);
    struct spinlock_rel_scope chk;

    bool irqs_enabled = irq_disable_save();

    if (checked_shallow)
        spinlock_shallow_validate_top(lock, old, site);
    spinlock_rel_begin(&chk, lock, site);

    spin_unlock_physical(lock);

    spinlock_rel_commit(&chk);
    if (checked_shallow)
        spinlock_shallow_pop(lock);

    spinlock_restore_interrupts(irqs_enabled);

    irql_lower(old);
}

static inline cc_warn_unused_result enum irql
spin_lock_subclass_full(struct spinlock *lock, uint8_t subclass,
                        const struct lock_chk_site *site)
    TSA_ACQUIRES_SPIN(lock) TSA_NO_ANALYSIS {
    enforce(subclass < LOCK_CHK_MAX_SUBCLASSES);
    if (bootstage_get() >= BOOTSTAGE_MID_MP &&
        (irq_in_interrupt() || irq_in_nmi()))
        panic("Attempted to take non-ISR safe spinlock outside thread context");

    enum lock_op_flags flags = LOCK_OP_KIND_BLOCKING | LOCK_OP_IRQ_DISPATCH;

    spinlock_note_use(lock, flags);
    bool checked_shallow = spinlock_order_checked(lock);
    if (checked_shallow)
        spinlock_classify(lock, flags, site);

    struct spinlock_acq_scope chk;
    spinlock_acq_begin(&chk, lock, site, subclass, flags);

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    spin_lock_physical(lock);

    bool irqs_enabled = irq_disable_save();

    if (checked_shallow)
        spinlock_shallow_push(lock, irql, site);
    spinlock_acq_commit(&chk);

    spinlock_restore_interrupts(irqs_enabled);

    return irql;
}

static inline cc_warn_unused_result enum irql
spin_lock_full(struct spinlock *lock, const struct lock_chk_site *site)
    TSA_ACQUIRES_SPIN(lock) TSA_NO_ANALYSIS {
    return spin_lock_subclass_full(lock, 0, site);
}

static inline cc_warn_unused_result enum irql
spin_lock_high_full(struct spinlock *lock, const struct lock_chk_site *site)
    TSA_ACQUIRES_SPIN(lock) TSA_NO_ANALYSIS {
    if (bootstage_get() >= BOOTSTAGE_MID_MP && irq_in_nmi())
        panic("Attempted to take non-raw spinlock from an NMI");

    enum lock_op_flags flags = LOCK_OP_KIND_BLOCKING | LOCK_OP_IRQ_HIGH;

    spinlock_note_use(lock, flags);
    bool checked_shallow = spinlock_order_checked(lock);
    if (checked_shallow)
        spinlock_classify(lock, flags, site);

    struct spinlock_acq_scope chk;
    spinlock_acq_begin(&chk, lock, site, 0, flags);

    enum irql irql = irql_raise(IRQL_HIGH_LEVEL);
    spin_lock_physical(lock);

    if (checked_shallow)
        spinlock_shallow_push(lock, irql, site);
    spinlock_acq_commit(&chk);

    return irql;
}

static inline cc_warn_unused_result bool
spin_trylock_full(struct spinlock *lock, enum irql *out,
                  const struct lock_chk_site *site)
    TSA_TRY_ACQUIRES_SPIN(true, lock) TSA_NO_ANALYSIS {
    if (bootstage_get() >= BOOTSTAGE_MID_MP &&
        (irq_in_interrupt() || irq_in_nmi()))
        panic("Attempted to take non-ISR safe spinlock outside thread context");

    enum lock_op_flags flags = LOCK_OP_KIND_TRY | LOCK_OP_IRQ_DISPATCH;

    spinlock_note_use(lock, flags);

    bool checked_shallow = spinlock_order_checked(lock);
    if (checked_shallow)
        spinlock_classify(lock, flags, site);

    struct spinlock_acq_scope chk;
    spinlock_acq_begin(&chk, lock, site, 0, flags);

    *out = irql_raise(IRQL_DISPATCH_LEVEL);
    if (spin_trylock_physical(lock)) {
        bool irqs_enabled = irq_disable_save();

        if (checked_shallow)
            spinlock_shallow_push(lock, *out, site);
        spinlock_acq_commit(&chk);

        spinlock_restore_interrupts(irqs_enabled);
        return true;
    }

    spinlock_acq_abort(&chk);
    irql_lower(*out);
    return false;
}

static inline cc_warn_unused_result bool
spin_trylock_high_full(struct spinlock *lock, enum irql *out,
                       const struct lock_chk_site *site)
    TSA_TRY_ACQUIRES_SPIN(true, lock) TSA_NO_ANALYSIS {
    if (bootstage_get() >= BOOTSTAGE_MID_MP && irq_in_nmi())
        panic("Attempted to take non-raw spinlock from an NMI");

    enum lock_op_flags flags = LOCK_OP_KIND_TRY | LOCK_OP_IRQ_HIGH;

    spinlock_note_use(lock, flags);
    bool checked_shallow = spinlock_order_checked(lock);
    if (checked_shallow)
        spinlock_classify(lock, flags, site);

    struct spinlock_acq_scope chk;
    spinlock_acq_begin(&chk, lock, site, 0, flags);

    *out = irql_raise(IRQL_HIGH_LEVEL);
    if (spin_trylock_physical(lock)) {
        if (checked_shallow)
            spinlock_shallow_push(lock, *out, site);
        spinlock_acq_commit(&chk);

        return true;
    }

    spinlock_acq_abort(&chk);
    irql_lower(*out);
    return false;
}

#define spin_lock(lock_) spin_lock_full((lock_), LOCK_CHK_SITE_HERE())
#define spin_lock_subclass(lock_, subclass_)                                   \
    spin_lock_subclass_full((lock_), (subclass_), LOCK_CHK_SITE_HERE())
#define spin_lock_high(lock_) spin_lock_high_full((lock_), LOCK_CHK_SITE_HERE())
#define spin_trylock(lock_, out_)                                              \
    spin_trylock_full((lock_), (out_), LOCK_CHK_SITE_HERE())
#define spin_trylock_high(lock_, out_)                                         \
    spin_trylock_high_full((lock_), (out_), LOCK_CHK_SITE_HERE())
#define spin_unlock(lock_, old_)                                               \
    spin_unlock_full((lock_), (old_), LOCK_CHK_SITE_HERE())
#define spin_lock_raw(lock_) spin_lock_raw_full((lock_), LOCK_CHK_SITE_HERE())
#define spin_trylock_raw(lock_)                                                \
    spin_trylock_raw_full((lock_), LOCK_CHK_SITE_HERE())
#define spin_unlock_raw(lock_)                                                 \
    spin_unlock_raw_full((lock_), LOCK_CHK_SITE_HERE())

static inline bool spinlock_locked(struct spinlock *lock) {
    return atomic_load_rlx(&lock->raw.state);
}

/* A raw check to make sure it is locked, but could be by anyone */
#define SPINLOCK_ASSERT_LOCKED(l)                                              \
    enforce(spinlock_locked(l), "spinlock not locked")

static inline void spinlock_set_chk_flags(struct spinlock *lock,
                                          enum lock_chk_flags flags) {
    enforce(!spinlock_locked(lock));
    spinlock_policy_set_internal(lock, flags);
}

static inline void spinlock_reinit_chk(struct spinlock *lock,
                                       const struct lock_chk_class *class,
                                       enum lock_chk_flags flags) {
    enforce(!spinlock_locked(lock));
    spinlock_assert_initialized(lock);
    spinlock_init_chk_full(lock, class, flags);
}

static inline void spinlock_assert_held_full(struct spinlock *lock,
                                             const struct lock_chk_site *site)
    TSA_ASSERT_CAPABILITY(lock) {
    if (spinlock_assert_held_deep(lock, /*want_held=*/true, site))
        return;

    SPINLOCK_ASSERT_LOCKED(lock);
}

static inline void
spinlock_assert_not_held_full(struct spinlock *lock,
                              const struct lock_chk_site *site) {
    if (spinlock_assert_held_deep(lock, /*want_held=*/false, site))
        return;

    /* Cannot check here */
    if (bootstage_get() < BOOTSTAGE_EARLY_DEVICES)
        enforce(!spinlock_locked(lock), "spinlock unexpectedly locked");
}

#define SPINLOCK_ASSERT_HELD(l)                                                \
    spinlock_assert_held_full((l), LOCK_CHK_SITE_HERE())
#define SPINLOCK_ASSERT_NOT_HELD(l)                                            \
    spinlock_assert_not_held_full((l), LOCK_CHK_SITE_HERE())

/* Scoped guards. TSA can't connect guard variables to lock expressions.
 * IRQL_RAISED is acquired on entry and released on cleanup, and
 * this prevents sleeping inside the guard. Lock expression is evaluated
 * twice, remember to pass a lock address */
struct spinlock_guard {
    struct spinlock *lock;
    enum irql irql;
};

static inline cc_always_inline cc_maybe_unused void
spinlock_guard_assume(struct spinlock *lock) TSA_ASSERT_CAPABILITY(lock) {
    cc_unused(lock);
}

static inline cc_always_inline cc_maybe_unused void spinlock_guard_exit(
    struct spinlock_guard *g) TSA_RELEASES_SPIN_IRQL TSA_NO_ANALYSIS {
    if (g->lock)
        spin_unlock(g->lock, g->irql);
}

static inline cc_always_inline cc_maybe_unused struct spinlock_guard
spin_guard_enter(struct spinlock *lock) TSA_ACQUIRES_SPIN_IRQL TSA_NO_ANALYSIS {
    return (struct spinlock_guard) {
        .lock = lock,
        .irql = spin_lock(lock),
    };
}

static inline cc_always_inline cc_maybe_unused struct spinlock_guard
spin_guard_high_enter(struct spinlock *lock)
    TSA_ACQUIRES_SPIN_IRQL TSA_NO_ANALYSIS {
    return (struct spinlock_guard) {
        .lock = lock,
        .irql = spin_lock_high(lock),
    };
}

static inline cc_always_inline cc_maybe_unused struct spinlock_guard
spin_guard_subclass_enter(struct spinlock *lock, uint8_t subclass)
    TSA_ACQUIRES_SPIN_IRQL TSA_NO_ANALYSIS {
    return (struct spinlock_guard) {
        .lock = lock,
        .irql = spin_lock_subclass(lock, subclass),
    };
}

#define spin_guard(lock_)                                                      \
    cc_cleanup(spinlock_guard_exit) struct spinlock_guard PP_CONCAT(           \
        spin_guard_, __COUNTER__) =                                            \
        (spinlock_guard_assume(lock_), spin_guard_enter(lock_))

#define spin_guard_high(lock_)                                                 \
    cc_cleanup(spinlock_guard_exit) struct spinlock_guard PP_CONCAT(           \
        spin_guard_high_, __COUNTER__) =                                       \
        (spinlock_guard_assume(lock_), spin_guard_high_enter(lock_))

#define spin_guard_subclass(lock_, subclass_)                                  \
    cc_cleanup(spinlock_guard_exit) struct spinlock_guard PP_CONCAT(           \
        spin_guard_, __COUNTER__) =                                            \
        (spinlock_guard_assume(lock_),                                         \
         spin_guard_subclass_enter((lock_), (subclass_)))
