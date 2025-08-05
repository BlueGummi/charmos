#include <compiler.h>
#define kassert(x)                                                             \
    do {                                                                       \
        if (!unlikely(x))                                                      \
            k_panic("Assertion " #x " failed");                                \
    } while (0)
