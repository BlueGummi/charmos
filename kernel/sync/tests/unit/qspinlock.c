#include "sync/tests/test_internal.h"

#include <test/fleet.h>

TEST_GROUP_DEFINE(qspinlock);

TEST_DEFINE_UNIT(qspinlock, tail_encoding) {
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
            TEST_ASSERT_EQ((tail & Q_SPIN_LOCKED_PENDING_MASK), 0);

            /* Extract and verify fields */
            cpu_id_t decoded_cpu = (tail >> Q_SPIN_TAIL_CPU_OFFSET) - 1;
            enum qspinlock_level decoded_lvl =
                (tail & Q_SPIN_TAIL_LVL_MASK) >> Q_SPIN_TAIL_LVL_OFFSET;

            TEST_ASSERT_EQ(decoded_cpu, cpu);
            TEST_ASSERT_EQ(decoded_lvl, lvl);
        }
    }

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(qspinlock, pending_to_locked_math) {
    /* Test the transition: lock has tail + pending bit, and adding
     * (Q_SPIN_LOCKED_VAL - Q_SPIN_PENDING_VAL) = -255 */
    uint32_t tail = (42 << Q_SPIN_TAIL_CPU_OFFSET) |
                    (QSPINLOCK_LEVEL_NORMAL << Q_SPIN_TAIL_LVL_OFFSET);
    uint32_t val = tail | Q_SPIN_PENDING_VAL;

    uint32_t next = val + (Q_SPIN_LOCKED_VAL - Q_SPIN_PENDING_VAL);

    /* Lock bit set, pending bit cleared, tail preserved */
    TEST_ASSERT_EQ((next & Q_SPIN_LOCKED_MASK), Q_SPIN_LOCKED_VAL);
    TEST_ASSERT_EQ((next & Q_SPIN_PENDING_MASK), 0);
    TEST_ASSERT_EQ((next & Q_SPIN_TAIL_MASK), tail);

    return TEST_SUCCESS;
}

#define QSPINLOCK_CONTENTION_THREADS 12
#define QSPINLOCK_CONTENTION_ITERS 300

struct qspin_contention_fix {
    struct qspinlock lock;
    atomic_size_t count;
};

static bool qspinlock_contention_worker(struct test_fleet *f,
                                        struct test_conc_worker *w) {
    struct qspin_contention_fix *fix = w->arg;
    cc_ignore(f);

    for (size_t i = 0; i < QSPINLOCK_CONTENTION_ITERS; i++) {
        enum irql irql = qspin_lock(&fix->lock);
        atomic_inc(&fix->count);
        qspin_unlock(&fix->lock, irql);
    }

    return true;
}

TEST_DEFINE_INTEGRATION(qspinlock, contended_handoff, .min_cores = 2) {
    struct test_fleet *fleet = test_fleet_init(ctx);

    struct qspin_contention_fix *fix = test_new(fix);
    fix->lock = (struct qspinlock) QSPINLOCK_INIT;

    for (size_t i = 0; i < QSPINLOCK_CONTENTION_THREADS; i++)
        test_fleet_spawn(fleet, "qspin_contend", qspinlock_contention_worker,
                         .arg = fix);

    test_fleet_start_all(fleet);

    struct test_verdict v = test_fleet_join(fleet);
    if (v.result != TEST_RESULT_OK)
        return v;

    TEST_ASSERT_EQ(atomic_load(&fix->count),
                   QSPINLOCK_CONTENTION_THREADS * QSPINLOCK_CONTENTION_ITERS);

    return TEST_SUCCESS;
}
