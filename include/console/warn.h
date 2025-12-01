#include <console/printf.h>
#include <stdatomic.h>

#define k_warn_once(msg, ...)                                                  \
    do {                                                                       \
        static atomic_bool __warned = false;                                   \
        if (atomic_exchange(&__warned, true) == false)                         \
            k_info("WARN", K_WARN, msg, ##__VA_ARGS__);                        \
    } while (0)
