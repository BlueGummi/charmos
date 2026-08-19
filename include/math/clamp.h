/* @title: Clamping Macros */
#pragma once
#include <kassert.h>
#define CLAMP(__var, __min, __max)                                             \
    do {                                                                       \
        kassert(__min < __max);                                                \
        if (__var > __max)                                                     \
            __var = __max;                                                     \
        if (__var < __min)                                                     \
            __var = __min;                                                     \
    } while (0)
