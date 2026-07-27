/* @title: Type Enum */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum type_enum {
    TYPE_NONE,
    TYPE_INT8,
    TYPE_UINT8,
    TYPE_INT16,
    TYPE_UINT16,
    TYPE_INT32,
    TYPE_UINT32,
    TYPE_INT64,
    TYPE_UINT64,
    TYPE_FLOAT32,
    TYPE_FLOAT64,
    TYPE_BOOL,
    TYPE_POINTER,
    TYPE_UNKNOWN,
    TYPE_MAX,
};

#define TYPE_TO_ENUM(X)                                                        \
    _Generic((X),                                                              \
        int8_t: TYPE_INT8,                                                     \
        uint8_t: TYPE_UINT8,                                                   \
        int16_t: TYPE_INT16,                                                   \
        uint16_t: TYPE_UINT16,                                                 \
        int32_t: TYPE_INT32,                                                   \
        uint32_t: TYPE_UINT32,                                                 \
        int64_t: TYPE_INT64,                                                   \
        uint64_t: TYPE_UINT64,                                                 \
        float: TYPE_FLOAT32,                                                   \
        double: TYPE_FLOAT64,                                                  \
        bool: TYPE_BOOL,                                                       \
        default: _Generic((void *) (X),                                        \
            void *: TYPE_POINTER,                                              \
            const void *: TYPE_POINTER,                                        \
            volatile void *: TYPE_POINTER,                                     \
            const volatile void *: TYPE_POINTER,                               \
            default: TYPE_UNKNOWN))

static inline size_t type_enum_width(enum type_enum type) {
    switch (type) {
    case TYPE_INT8:
    case TYPE_UINT8: return 1;
    case TYPE_INT16:
    case TYPE_UINT16: return 2;
    case TYPE_INT32:
    case TYPE_UINT32:
    case TYPE_FLOAT32: return 4;
    case TYPE_INT64:
    case TYPE_UINT64:
    case TYPE_FLOAT64: return 8;
    case TYPE_BOOL: return 1;
    case TYPE_POINTER: return sizeof(void *);
    default: return 0;
    }
}

static inline const char *type_enum_str(enum type_enum type) {
    switch (type) {
    case TYPE_NONE: return "none";
    case TYPE_INT8: return "int8";
    case TYPE_UINT8: return "uint8";
    case TYPE_INT16: return "int16";
    case TYPE_UINT16: return "uint16";
    case TYPE_INT32: return "int32";
    case TYPE_UINT32: return "uint32";
    case TYPE_INT64: return "int64";
    case TYPE_UINT64: return "uint64";
    case TYPE_FLOAT32: return "float32";
    case TYPE_FLOAT64: return "float64";
    case TYPE_BOOL: return "bool";
    case TYPE_POINTER: return "ptr";
    default: return "unknown";
    }
}
