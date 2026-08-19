/* @title: Range Macros */
#pragma once
#include <kassert.h>
#define IN_RANGE(x, min, max)                                                  \
    ({                                                                         \
        (void) kassert((min) <= (max));                                        \
        (x) >= (min) && (x) <= (max);                                          \
    })
