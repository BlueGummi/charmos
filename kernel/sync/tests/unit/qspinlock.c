#include "../test_internal.h"

#ifdef TEST_QSPINLOCK
TEST_GROUP_DECLARE(qspinlock);

TEST_DECLARE_UNIT(qspinlock, qspinlock_tail_encoding) {
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

TEST_DECLARE_UNIT(qspinlock, qspinlock_pending_to_locked_math) {
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

static struct qspinlock qspinlock_contention_lock = QSPINLOCK_INIT;
static atomic_bool qspinlock_contention_start = false;
static _Atomic size_t qspinlock_contention_count = 0;

static void qspinlock_contention_worker(void *) {
    while (!atomic_load(&qspinlock_contention_start))
        cpu_relax();

    for (size_t i = 0; i < QSPINLOCK_CONTENTION_ITERS; i++) {
        enum irql irql = qspin_lock(&qspinlock_contention_lock);
        atomic_fetch_add(&qspinlock_contention_count, 1);
        qspin_unlock(&qspinlock_contention_lock, irql);
    }
}

TEST_DECLARE_INTEGRATION(qspinlock, qspinlock_contended_handoff) {
    if (global.core_count < 2)
        return TEST_SKIP(TEST_SKIP_NONE);

    atomic_store(&qspinlock_contention_start, false);
    atomic_store(&qspinlock_contention_count, 0);

    struct thread *workers[QSPINLOCK_CONTENTION_THREADS];
    for (size_t i = 0; i < QSPINLOCK_CONTENTION_THREADS; i++) {
        workers[i] = thread_spawn_joinable("qspin_contend",
                                           qspinlock_contention_worker, NULL);
        TEST_ASSERT_NONNULL(workers[i]);
    }

    atomic_store(&qspinlock_contention_start, true);

    for (size_t i = 0; i < QSPINLOCK_CONTENTION_THREADS; i++)
        thread_join(workers[i]);

    TEST_ASSERT_EQ(atomic_load(&qspinlock_contention_count),
                   QSPINLOCK_CONTENTION_THREADS * QSPINLOCK_CONTENTION_ITERS);

    return TEST_SUCCESS;
}
#endif
