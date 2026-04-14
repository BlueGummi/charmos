/* @title: To bits, to bytes */
#pragma once
#include <stddef.h>

static inline size_t to_bits(size_t bytes) {
    return bytes * 8;
}

static inline size_t to_bytes(size_t bits) {
    return (bits + 7) / 8;
}
