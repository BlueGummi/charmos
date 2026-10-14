/* @title: General Locking */
#pragma once
#include <compiler/name.h>
#include <stdint.h>

/* The reason we have this is because code around the kernel uses a function
 * parameter to indicate the held state of a lock. Using this enum rather than
 * booleans avoids ad-hoc annotations at callsites, assigns explicit names, and
 * improves readability and searchability. */
enum lock_held_state {
    /* We're intentionally using truthy and falsy compatible values here:
     * much code assumes true == locked, and this will remain stable so
     * only the signatures and types change, and code can still go
     *
     * `if (held) do_thing();` */
    LOCK_NOT_HELD = 0,
    LOCK_HELD = 1,
};

/* for breaking ordering cycles between locks of the same class
 * max is LOCK_CHK_MAX_SUBCLASSES (8) */
enum lock_subclass : uint8_t {
    LOCK_SUBCLASS_PRIMARY = 0,
    LOCK_SUBCLASS_SECONDARY = 1,
    LOCK_SUBCLASS_TERTIARY = 2,

    /* aliases */
    LOCK_SUBCLASS_PARENT = 0,
    LOCK_SUBCLASS_CHILD = 1,

    LOCK_SUBCLASS_SRC_CPU = 0,
    LOCK_SUBCLASS_DST_CPU = 1,
};

enum lock_acquire_policy {
    LOCK_ACQUIRE_BLOCKING = 0,
    LOCK_ACQUIRE_NONBLOCKING = 1,
};
