#define kassert(x)                                                             \
    do {                                                                       \
        if (!(x))                                                              \
            k_panic("Assertion " #x " failed");                                \
    } while (0)
