#include "../test_internal.h"

#include <asm.h>
#include <sync/raw_spinlock.h>

#ifdef TEST_QSPINLOCK
TEST_DECLARE_UNIT(qspinlock, raw_spinlock_physical_operations) {
    struct raw_spinlock lock = RAW_SPINLOCK_INIT;

    raw_spin_lock(&lock);
    bool rejected_while_held = !raw_spin_trylock(&lock);
    raw_spin_unlock(&lock);

    bool acquired_after_release = raw_spin_trylock(&lock);
    raw_spin_unlock(&lock);

    TEST_ASSERT(rejected_while_held);
    TEST_ASSERT(acquired_after_release);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(qspinlock, raw_spinlock_irq_restore) {
    struct raw_spinlock lock = RAW_SPINLOCK_INIT;
    bool entry_irqs_enabled = are_interrupts_enabled();

    bool saved_irqs_enabled = raw_spin_lock_irq_disable(&lock);
    bool disabled_while_held = !are_interrupts_enabled();
    raw_spin_unlock_irq_restore(&lock, saved_irqs_enabled);
    bool restored_entry_state = are_interrupts_enabled() == entry_irqs_enabled;

    disable_interrupts();
    bool saved_disabled_state = raw_spin_lock_irq_disable(&lock);
    raw_spin_unlock_irq_restore(&lock, saved_disabled_state);
    bool remained_disabled = !are_interrupts_enabled();

    if (entry_irqs_enabled)
        enable_interrupts();

    TEST_ASSERT_EQ(saved_irqs_enabled, entry_irqs_enabled);
    TEST_ASSERT(disabled_while_held);
    TEST_ASSERT(restored_entry_state);
    TEST_ASSERT(!saved_disabled_state);
    TEST_ASSERT(remained_disabled);
    return TEST_SUCCESS;
}
#endif
