#pragma once
#include <err.h>
#include <math/fixed.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ELCM_MAX_WASTAGE_DEFAULT 1

struct elcm_candidate {
    size_t pages;
    size_t wasted; /* bytes; negative when metadata_size_bytes exceeds slack */
    fx32_32_t wastage; /* fraction in [0, 1) */
    size_t distance;
    fx32_32_t score_value;
    size_t obj_count;
    size_t bitmap_bytes;
    size_t metadata_bytes;
    size_t obj_size;
    size_t obj_alignment;
};

struct elcm_params {
    size_t obj_alignment;
    size_t obj_size;
    size_t max_wastage_pct;
    size_t max_pages;
    bool bias_towards_pow2;
    size_t metadata_size_bytes;
    size_t metadata_bits_per_obj;
    size_t metadata_bytes_per_page;

    /* If NULL, defaults to valloc */
    void *(*alloc_fn)(size_t size);
    void (*free_fn)(void *ptr, size_t size);
    struct elcm_candidate out;
};

enum err elcm(struct elcm_params *params);
