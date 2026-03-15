/* @title: Realtime scheduling types */
#pragma once
#include <stdint.h>
/* This enum defines *what* the realtime scheduler will tell you from
 * functions. For example, when it summarizes itself and produces a
 * `struct rt_thread_summary` it will also send along an error
 * with it, denoting any internal happening, e.g. not being able
 * to migrate threads because of deadline reasons... */
enum rt_scheduler_error {
    /* Ranges:
     *
     * [-20, -11] = general failures
     *
     * [-10, -1] = migration failures
     * 0 = success (no message)
     * [1, 10] = migration success with message
     *
     * [11, 20] = general success with message
     */

    RT_SCHEDULER_ERR_FAIL_ASAP = -20, /* Tell the core scheduler
                                       * to fail the RT scheduler */

    RT_SCHEDULER_ERR_INCOMPATIBLE = -19, /* No compatible CPU
                                          * found for thread */

    RT_SCHEDULER_ERR_SWITCH_IMPOSSIBLE = -18, /* Switching the scheduler would
                                               * lead to unhoused threads */

    RT_SCHEDULER_ERR_OOM = -17, /* Specifically ran out of memory */

    RT_SCHEDULER_ERR_OOR = -16,       /* Generic "Out of resources" */
    RT_SCHEDULER_ERR_NOT_FOUND = -15, /* cannot find it */
    RT_SCHEDULER_ERR_INVALID = -14,   /* Invalid operation */
    RT_SCHEDULER_ERR_UNKNOWN = -13,

    RT_SCHEDULER_ERR_POLICY = -3,
    RT_SCHEDULER_ERR_DEADLINE = -2, /* Deadline-related error */

    RT_SCHEDULER_ERR_AFFINITY = -1, /* All threads are pinned/
                                     * unmigratable... so there's
                                     * nothing we can do anyways */

    RT_SCHEDULER_ERR_OK = 0,

};

/* rt_scheduler_capability: 16 bit bitflags:
 *
 *      ┌───────────────────────────┐
 * Bits │ 15..12  11..8  7..4  3..0 │
 * Use  │  X***    ****  ***M  DERF │
 *      └───────────────────────────┘
 *
 * F - First In, First Out
 * R - Round-Robin
 * E - Earliest Deadline First
 * D - Deadline Capable
 * M - Migration Capable
 * X - !!! FAULT TOLERANT !!! ALL THREADS UNDER THIS SCHEDULER
 *     MUST ALSO BE FAULT TOLERANT, ELSE THEY WILL NOT RUN!!!
 * * - Unused, but not reserved
 *
 */
enum rt_scheduler_capability : uint16_t {
    RT_CAP_FIFO = 1 << 0,
    RT_CAP_RR = 1 << 1,
    RT_CAP_EDF = 1 << 2,
    RT_CAP_DEADLINE = 1 << 3,
    RT_CAP_MIGRATABLE = 1 << 4,
    RT_CAP_FAULT_TOLERANT = 1 << 15,
};
