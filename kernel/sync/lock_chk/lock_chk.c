#include <atomic.h>
#include <bootstage.h>
#include <cmdline.h>
#include <enforce.h>
#include <sync/lock_chk.h>

#ifdef DEBUG_LOCK_CHK

#include "internal.h"

struct lock_chk_globals lock_chk_global = {0};

CMDLINE_DEFINE_STATIC(lock_chk, .flags = CMDLINE_ENTRY_SYMBOLIC,
                      .desc = "Lock validator command line namespace");

CMDLINE_CHILDREN_DEFINE(
    lock_chk,
    CMDLINE_INNER_VAR(
        panic_on_exhaustion, lock_chk_global.panic_on_exhaustion,
        .default_val = "true",
        .desc = "Panic when lock validator capacity is exhausted"),
    CMDLINE_INNER_VAR(
        capacity_panic, lock_chk_global.panic_on_exhaustion,
        .default_val = "true",
        .desc = "Panic when lock validator capacity is exhausted (alias)"));

void lock_chk_init(void) {
    enforce(bootstage_get() >= BOOTSTAGE_LATE);
    lock_chk_deep_activate();
    lock_debug_activate();
    atomic_store_release(&lock_chk_global.state, LOCK_CHK_ACTIVE);
}

bool lock_chk_tracking_active(void) {
    return lock_chk_state_active(&lock_chk_global.state);
}

void lock_chk_note_use(struct lock_chk_lock *lock, enum lock_op_flags flags) {
    if (!lock_chk_tracking_active())
        return;

    bool manages_irql = (flags & LOCK_OP_IRQ_MASK) != 0;
    bool raw = flags & LOCK_OP_RAW;
    enforce(lock->initialized);
    enforce((lock->flags & ~LOCK_CHKD_FULL) == 0);
    if (manages_irql && !raw)
        enforce((lock->flags & LOCK_CHKD_THREAD) == 0 ||
                (lock->flags & LOCK_CHKD_ORDER) != 0);

    atomic_store_release(&lock->used, true);
}

#else /* !defined(DEBUG_LOCK_CHK) */

void lock_chk_init(void) {}

bool lock_chk_tracking_active(void) {
    return false;
}

#endif /* DEBUG_LOCK_CHK */
