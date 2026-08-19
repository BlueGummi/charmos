#include "../test_internal.h"

#ifdef TEST_QSPINLOCK
TEST_GROUP_DECLARE(qspinlock);

TEST_DECLARE_UNIT(qspinlock_tail_encoding, .group = TEST_GROUP(qspinlock)) {
    /* Test tail encoding across CPUs and context levels */
    cpu_id_t cpus[] = {0, 1, 15, 255, 1024, 65534};
    enum qspinlock_level levels[] = {QSPINLOCK_LEVEL_NORMAL,
                                     QSPINLOCK_LEVEL_IRQ};

    for (size_t c = 0; c < sizeof(cpus) / sizeof(cpus[0]); c++) {
        for (size_t l = 0; l < sizeof(levels) / sizeof(levels[0]); l++) {
            cpu_id_t cpu = cpus[c];
            enum qspinlock_level lvl = levels[l];

            uint32_t tail = ((cpu + 1) << Q_SPIN_TAIL_CPU_OFFSET) |
                            (lvl << Q_SPIN_TAIL_LVL_OFFSET);

            /* Tail bits must not overlap locked byte or pending bit */
            TEST_ASSERT((tail & Q_SPIN_LOCKED_PENDING_MASK) == 0);

            /* Extract and verify fields */
            cpu_id_t decoded_cpu = (tail >> Q_SPIN_TAIL_CPU_OFFSET) - 1;
            enum qspinlock_level decoded_lvl =
                (tail & Q_SPIN_TAIL_LVL_MASK) >> Q_SPIN_TAIL_LVL_OFFSET;

            TEST_ASSERT(decoded_cpu == cpu);
            TEST_ASSERT(decoded_lvl == lvl);
        }
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(qspinlock_pending_to_locked_math,
                  .group = TEST_GROUP(qspinlock)) {
    /* Test the transition: lock has tail + pending bit, and adding
     * (Q_SPIN_LOCKED_VAL - Q_SPIN_PENDING_VAL) = -255 */
    uint32_t tail = (42 << Q_SPIN_TAIL_CPU_OFFSET) |
                    (QSPINLOCK_LEVEL_NORMAL << Q_SPIN_TAIL_LVL_OFFSET);
    uint32_t val = tail | Q_SPIN_PENDING_VAL;

    uint32_t next = val + (Q_SPIN_LOCKED_VAL - Q_SPIN_PENDING_VAL);

    /* Lock bit set, pending bit cleared, tail preserved */
    TEST_ASSERT((next & Q_SPIN_LOCKED_MASK) == Q_SPIN_LOCKED_VAL);
    TEST_ASSERT((next & Q_SPIN_PENDING_MASK) == 0);
    TEST_ASSERT((next & Q_SPIN_TAIL_MASK) == tail);

    return TEST_SUCCESS;
}
#endif
