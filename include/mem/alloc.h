#pragma once
#define PAGE_SIZE 4096
#include <stdbool.h>
#include <stdint.h>

#define ALLOC_LOCALITY_SHIFT 8
#define ALLOC_CLASS_SHIFT 12
#define ALLOC_CLASS_MASK 0xF

#define ALLOC_LOCALITY_MAX 7
#define ALLOC_LOCALITY_MIN 0
#define ALLOC_LOCALITY_MASK 0x7

#define ALLOC_LOCALITY_FROM_FLAGS(flags)                                       \
    (((flags) >> ALLOC_LOCALITY_SHIFT) & ALLOC_LOCALITY_MASK)

#define ALLOC_LOCALITY_TO_FLAGS(locality)                                      \
    (((locality) & ALLOC_LOCALITY_MASK) << ALLOC_LOCALITY_SHIFT)

#define ALLOC_FLAG_TEST(flags, mask) (flags & mask)
#define ALLOC_FLAG_CLASS(flags)                                                \
    ((flags >> ALLOC_CLASS_SHIFT) & ALLOC_CLASS_MASK)

/* alloc_flags: 16 bit bitflags
 *
 *      +---------------------------+
 * Bits | 15--12  11--8  7--4  3--0 |
 * Use  |  %%%%    *###  mMpP  fFcC |
 *      +---------------------------+
 *
 * C - "Prefer cache alignment"
 * c - "Do not prefer cache alignment"
 *
 * F - "Allow flexible NUMA locality"
 * f - "Do not allow flexible NUMA locality"
 *
 * P - "Allow memory to be pageable"
 * p - "Do not allow memory to be pageable"
 *
 * M - "Allow memory to be movable"
 * m - "Do not allow memory to be movable"
 *
 * ### - Locality bits
 * * - Unused
 * %%%% - Allocation class bits
 *
 */

enum alloc_flags : uint16_t {
    /* Cache alignment */
    ALLOC_FLAG_PREFER_CACHE_ALIGNED = (1 << 0),
    ALLOC_FLAG_NO_CACHE_ALIGN = (1 << 1),

    /* Flexible locality */
    ALLOC_FLAG_FLEXIBLE_LOCALITY = (1 << 2),
    ALLOC_FLAG_STRICT_LOCALITY = (1 << 3),

    /* Pageable */
    ALLOC_FLAG_PAGEABLE = (1 << 4),
    ALLOC_FLAG_NONPAGEABLE = (1 << 5),

    /* Movable */
    ALLOC_FLAG_MOVABLE = (1 << 6),
    ALLOC_FLAG_NONMOVABLE = (1 << 7),

    /* Allocation classes */
    ALLOC_FLAG_CLASS_DEFAULT = (0 << ALLOC_CLASS_SHIFT),
    ALLOC_FLAG_CLASS_INTERLEAVED = (1 << ALLOC_CLASS_SHIFT),
    ALLOC_FLAG_CLASS_HIGH_BANDWIDTH = (2 << ALLOC_CLASS_SHIFT),
};

#define ALLOC_FLAGS_NONE                                                       \
    (ALLOC_FLAG_CLASS_DEFAULT | ALLOC_FLAG_FLEXIBLE_LOCALITY |                 \
     ALLOC_FLAG_NONMOVABLE | ALLOC_FLAG_NONPAGEABLE |                          \
     ALLOC_FLAG_NO_CACHE_ALIGN)

static inline bool alloc_flags_valid(uint16_t flags) {
    if ((flags & ALLOC_FLAG_FLEXIBLE_LOCALITY) &&
        (flags & ALLOC_FLAG_STRICT_LOCALITY))
        return false;

    if ((flags & ALLOC_FLAG_PAGEABLE) && (flags & ALLOC_FLAG_NONPAGEABLE))
        return false;

    if ((flags & ALLOC_FLAG_MOVABLE) && (flags & ALLOC_FLAG_NONMOVABLE))
        return false;

    if ((flags & ALLOC_FLAG_PREFER_CACHE_ALIGNED) &&
        (flags & ALLOC_FLAG_NO_CACHE_ALIGN))
        return false;

    return true;
}

void *kmalloc(uint64_t size);
void *krealloc(void *ptr, uint64_t size);
void *kzalloc(uint64_t size);
void kfree(void *ptr);
uint64_t ksize(void *ptr);
void *kmalloc_aligned(uint64_t size, uint64_t align);
void *kzalloc_aligned(uint64_t size, uint64_t align);
void kfree_aligned(void *ptr);

#pragma once
