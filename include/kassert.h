/* @title: Assertions */
#include <compiler.h>
#include <console/panic.h>
#define kassert(x)                                                             \
    do {                                                                       \
        if (!unlikely(x))                                                      \
            k_panic("Assertion " #x " failed\n");                              \
    } while (0)

#define kassert_unreachable() kassert("unreachable")
