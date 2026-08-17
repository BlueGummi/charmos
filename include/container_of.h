/* @title: container_of macro */
#pragma once
#include <compiler.h>
#define container_of(ptr, type, member)                                        \
    ({                                                                         \
        static_assert(__builtin_types_compatible_p(                            \
                          typeof(*(ptr)), typeof(((type *) 0)->member)),       \
                      "Incompatible types for containerof");                   \
        (type *) ((size_t) ptr - offsetof(type, member));                      \
    })
