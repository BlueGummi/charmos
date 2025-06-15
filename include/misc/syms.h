#pragma once
#include <stdint.h>

struct sym {
    uint64_t addr;
    const char *name;
};

extern const struct sym syms[];
extern const uint64_t syms_len; 
