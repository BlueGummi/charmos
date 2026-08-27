#include <bootstage.h>
#include <cmdline.h>
#include <kassert.h>
#include <stdatomic.h>
#include <sync/lock_chk.h>

#ifdef DEBUG_LOCK_CHK

#include "lock_chk_internal.h"

static _Atomic enum lock_chk_engine_state lock_chk_state = LOCK_CHK_INACTIVE;
static bool lock_chk_panic_on_exhaustion = true;

static CMDLINE_DECLARE(lock_chk, .flags = CMDLINE_ENTRY_SYMBOLIC,
                       .desc = "Lock validator command line namespace");

CMDLINE_CHILDREN_DECLARE(
    lock_chk,
    CMDLINE_INNER_VAR(
        panic_on_exhaustion, lock_chk_panic_on_exhaustion,
        .default_val = "true",
        .desc = "Panic when lock validator capacity is exhausted"),
    CMDLINE_INNER_VAR(
        capacity_panic, lock_chk_panic_on_exhaustion, .default_val = "true",
        .desc = "Panic when lock validator capacity is exhausted (alias)"));

void lock_chk_init(void) {
    kassert(bootstage_get() >= BOOTSTAGE_LATE);
    lock_chk_deep_activate();
    lock_debug_activate();
    atomic_store_explicit(&lock_chk_state, LOCK_CHK_ACTIVE,
                          memory_order_release);
}

bool lock_chk_tracking_active(void) {
    return atomic_load_explicit(&lock_chk_state, memory_order_acquire) ==
           LOCK_CHK_ACTIVE;
}

bool lock_chk_capacity_should_panic(void) {
    return lock_chk_panic_on_exhaustion;
}

void lock_chk_note_lock_use(bool initialized, enum lock_chk_flags flags,
                            _Atomic bool *used, bool manages_irql,
                            bool raw_operation) {
    if (!lock_chk_tracking_active())
        return;

    kassert(initialized);
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    if (manages_irql && !raw_operation)
        kassert((flags & LOCK_CHKD_THREAD) == 0 ||
                (flags & LOCK_CHKD_ORDER) != 0);

    atomic_store_explicit(used, true, memory_order_release);
}

#else /* !defined(DEBUG_LOCK_CHK) */

void lock_chk_init(void) {}

bool lock_chk_tracking_active(void) {
    return false;
}

#endif /* DEBUG_LOCK_CHK */
