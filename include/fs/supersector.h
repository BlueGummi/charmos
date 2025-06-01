#include <stdint.h>
#include <fs/detect.h>

struct generic_supersector {
    enum fs_type type;
    void *supersector;
    void (*print)(struct generic_supersector*);
};

#pragma once
