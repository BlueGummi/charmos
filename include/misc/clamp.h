#define CLAMP(__var, __min, __max)                                             \
    if (__var > __max)                                                         \
        __var = __max;                                                         \
    if (__var < __min)                                                         \
        __var = __min;

