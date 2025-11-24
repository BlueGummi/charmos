/* @title: IRQLs */

/* @idea:big IRQLs */
/* # Big Idea: IRQLs (STABLE)
 *
 * # Credits: gummi
 *
 * # Audience:
 *   Everyone
 *
 *   > IRQLs play a major role in the preemption and interrupt
 *     control mechanisms of this kernel. Thus, this document regarding
 *     the details of the IRQLs and usages is quite important.
 *
 * # Overview:
 *   IRQLs provide a centralized preemption and interrupt control mechanism.
 *   IRQLs are software only, meaning they are strictly enforced only by
 *   software and used as an abstraction in software.
 *
 * # Background:
 *   IRQLs are a feature of quite a few other kernels. Windows NT is the
 *   most prominent of the many kernels that have IRQLs. Similar concepts exist
 *   in other kernels, however. For example, VMS had 
 *
 * # Summary:
 *   Each logical processor (or CPU) on the machine has its own IRQL.
 *
 *   At each IRQL, the effects of lower IRQLs are also applied.
 *   There are 5 IRQLs, ordered from the least restrictive to most restrictive:
 *
 *     - PASSIVE (standard code execution, nothing blocked)
 *     - APC (APCs[^1] blocked)
 *     - DISPATCH (DPCs[^2] blocked, preemption blocked)
 *     - DEVICE (same as DISPATCH, currently reserved for future use)
 *     - HIGH (hardware interrupts blocked)
 *
 *   To change the IRQL, a function call to raise or lower it must be made.
 *   This is because the raise/lower operation must invoke other functions
 *   to block other events (such as preemption or interrupts). This means
 *   that you cannot change the current IRQL willy-nilly and expect everything
 *   to work out just fine.
 *
 *   ## Bootstage exception:
 *
 *   There is also another "pseudo-IRQL" that is used as a placeholder so that
 *   IRQL functions can be called at early bootstages[^3] (making the IRQL
 *   functions effectively no-ops), which is NONE.
 *
 * # API:
 *   There are three primary IRQL related functions:
 *
 *   `irql_raise()`, `irql_lower()`, and `irql_get()`.
 *
 *   `irql_raise()` raises the IRQL to a new IRQL, returning
 *   the previous IRQL. If the machine has not reached the LATE_DEVICES
 *   bootstage, it will not do anything and will return the NONE IRQL.
 *
 *   If this new IRQL is equal to the current IRQL, nothing is done.
 *   However, if the new IRQL is *lower* than the current IRQL, the function
 *   will panic.
 *
 *   `irql_lower()` lowers the IRQL to a new IRQL. If the bootstage has
 *   not reached the LATE_DEVICES stage, *or* if the NONE IRQL is passed in,
 *   the function will not do anything.
 *
 *   If the new IRQL is equal to the current IRQL, nothing is done.
 *   However, if the new IRQL is *higher* than the current IRQL, the function
 *   will panic.
 *
 *   `irql_lower()` will also check if any APCs and DPCs have came in, and
 *   execute them if the new IRQL permits the execution of these procedures.
 *
 *   `irql_lower()` will also check if a reschedule is needed, and appropriately
 *   call `scheduler_yield()` if preemption is enabled.
 *
 *   An example of a common IRQL usage pattern for disabling preemption:
 *
 *   ```c
 *   enum irql old_irql = irql_raise(IRQL_DISPATCH_LEVEL); // disable preemption
 *
 *   // do critical work
 *
 *   irql_lower(old_irql);
 *   ```
 *
 *   `irql_get()` will return the current IRQL of the caller's logical
 *   processor.
 *
 * # Errors:
 *   No errors besides the possible panics described above are possible.
 *
 * # Context:
 *   IRQL usage interacts with many subsystems, but primarily interacts with
 *   synchronization, interrupt handlers, and scheduling.
 *
 * # Constraints:
 *   The `irql_raise()` and `irql_lower()` functions cannot call themselves.
 *   Thus, inside the functions, whenever interrupts need to be disabled,
 *   they use the raw `disable_interrupts()` and `enable_interrupts()`
 *   assembly routines.
 *
 * # Internals:
 *   Whenever a thread raises the IRQL above PASSIVE, the thread will be pinned
 *   to the current core it is executing on. Then, when the IRQL is lowered
 *   to PASSIVE, the thread is unpinned if it was not pinned prior to raising
 *   the IRQL. This is done to prevent a thread from getting migrated at
 *   an IRQL above PASSIVE and modifying the IRQL of a different logical
 *   processor when it lowers the IRQL.
 *
 * # Strategy:
 *   To raise the IRQL, if we are not inside of an ISR, we first
 *   temporarily pin the running thread, and check if interrupts are enabled,
 *   and then disable interrupts, before unpinning the running thread.
 *
 *   This is done because the IRQL function operates on per-core variables,
 *   and thus, if it is interrupted and run again by another thread or an ISR,
 *   an invalid state can be read.
 *
 *   The pin-check-disable-unpin prevents the thread from being migrated in the
 *   window in between the check and disable operations.
 *
 *   Then, the necessary operations are performed depending on the IRQL being
 *   raised to. (disable preemption, interrupts, etc.).
 *
 *   To lower the IRQL, we simply re-enable the blocked event types, and
 *   attempt DPC and APC execution if we are currently running in the
 *   context of a thread.
 *
 * # Rationale:
 *   IRQLs were introduced as the preemption/interrupt control mechanism
 *   of this kernel because of the structure and strict rules they
 *   provide and enforce. DPCs and APCs also require IRQLs.
 *
 * # Changelog:
 *   11/22/2025 - gummi: created file
 *   11/23/2025 - gummi: move references
 *
 * # Notes:
 *   I primarily decided to bring IRQLs to this kernel because I had read
 *   code from kernels without IRQLs (with the constant
 *   enable/disable_interrupts, preempt_disable_enable sprinkled about),
 *   and thought "This could really be simplified with a state machine!",
 *   but then realized that VMS and NT basically did that already.
 *   So, I chose to introduce IRQLs.
 *
 *   [^1]: "APCs"
 *   [^2]: "DPCs"
 *   [^3]: "Bootstages"
 *
 */

#pragma once

/* We use a bit of the IRQL variable upon raising
 * and lowering the IRQL to identify if the thread was pinned
 * prior to the raise to pin threads on IRQL changes */

#define IRQL_MARK_THREAD_PINNED(irql) (irql |= (1ULL << 5ULL))
#define IRQL_THREAD_PINNED_SHIFT 5
#define IRQL_THREAD_PINNED(irql) ((irql >> 5ULL) & 1ULL)

#define IRQL_IRQL_MASK 0b1111

enum irql {
    IRQL_PASSIVE_LEVEL = 0,  /* Normal execution */
    IRQL_APC_LEVEL = 1,      /* Allow only high interrupts */
    IRQL_DISPATCH_LEVEL = 2, /* Allow higher than DPC interrupts */
    IRQL_DEVICE_LEVEL = 3,   /* Device interrupts */
    IRQL_HIGH_LEVEL = 4,     /* All interrupts masked */
    IRQL_NONE = -1,
};

static inline const char *irql_to_str(enum irql level) {
    switch (level) {
    case IRQL_PASSIVE_LEVEL: return "PASSIVE LEVEL";
    case IRQL_APC_LEVEL: return "APC LEVEL";
    case IRQL_DISPATCH_LEVEL: return "DISPATCH LEVEL";
    case IRQL_DEVICE_LEVEL: return "DEVICE LEVEL";
    case IRQL_HIGH_LEVEL: return "HIGH LEVEL";
    case IRQL_NONE: return "NONE";
    }
    return "UNKNOWN";
}

enum irql irql_raise(enum irql new_level);
void irql_lower(enum irql old_level);
enum irql irql_get();
