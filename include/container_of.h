/* @title: container_of macro */
#pragma once
#include <compiler/core.h>
#include <compiler/intrinsic.h>
#define container_of(ptr, type, member)                                        \
    ({                                                                         \
        static_assert(ci_types_compatible_p(typeof(*(ptr)),                    \
                                            typeof(((type *) 0)->member)),     \
                      "Incompatible types for container_of");                  \
        (type *) ((size_t) ptr - offsetof(type, member));                      \
    })
