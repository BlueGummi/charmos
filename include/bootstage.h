#include <stdint.h>
#pragma once

enum bootstage : uint8_t {
    BOOTSTAGE_EARLY_FB, /* Console can be printed to */

    BOOTSTAGE_EARLY_MP, /* APs are brought up and spinning */

    BOOTSTAGE_EARLY_ALLOCATORS, /* Early non-topology aware
                                 * allocators available */

    BOOTSTAGE_EARLY_DEVICES, /* Early devices (ACPI, LAPIC, HPET) brought up */

    BOOTSTAGE_MID_SCHEDULER, /* Scheduler and threads brought up */

    BOOTSTAGE_MID_MP, /* APs exit busy-spin and enter idle thread,
                       * topology is initialized and available */

    BOOTSTAGE_LATE_DEVICES, /* Rest of kernel is brought up -- filesystems,
                             * drivers, etc. almost all
                             * features are available in APIs */

    BOOTSTAGE_COMPLETE, /* Complete - enter init */
};
