/* @title: Assertions */
#include <compiler.h>
#include <console/panic.h>
#define kassert(x)                                                             \
    do {                                                                       \
        if (!unlikely(x)) {                                                    \
            panic("Assertion " #x " failed\n");                                \
            __builtin_unreachable();                                           \
        }                                                                      \
    } while (0)

#define kassert_unreachable()                                                  \
    do {                                                                       \
        kassert("unreachable");                                                \
        __builtin_unreachable();                                               \
    } while (0)
