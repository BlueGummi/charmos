/* @title: Hash Functions (primarily for hashmap/sets) */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Dan Bernstein's algorithm */
static inline uint32_t hash_djb2(const void *key, size_t len) {
    const uint8_t *data = (const uint8_t *) key;
    uint32_t hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + data[i];
    }
    return hash;
}

/* SDBM dadtabse */
static inline uint32_t hash_sdbm(const void *key, size_t len) {
    const uint8_t *data = (const uint8_t *) key;
    uint32_t hash = 0;
    for (size_t i = 0; i < len; i++) {
        hash = data[i] + (hash << 6) + (hash << 16) - hash;
    }
    return hash;
}

/* Fowler-Noll-Vo */
static inline uint32_t hash_fnv1a(const void *key, size_t len) {
    const uint8_t *data = (const uint8_t *) key;
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

static inline uint32_t hash_jenkins_one_at_a_time(const void *key, size_t len) {
    const uint8_t *data = (const uint8_t *) key;
    uint32_t hash = 0;
    for (size_t i = 0; i < len; i++) {
        hash += data[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

static inline uint32_t hash_murmur3_32(const void *key, size_t len,
                                       uint32_t seed) {
    const uint8_t *data = (const uint8_t *) key;
    const int nblocks = len / 4;
    uint32_t h1 = seed;

    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;

    const uint32_t *blocks = (const uint32_t *) (data + nblocks * 4);
    for (int i = -nblocks; i; i++) {
        uint32_t k1 = blocks[i];

        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;

        h1 ^= k1;
        h1 = (h1 << 13) | (h1 >> 19);
        h1 = h1 * 5 + 0xe6546b64;
    }

    const uint8_t *tail = (const uint8_t *) (data + nblocks * 4);
    uint32_t k1 = 0;
    switch (len & 3) {
    case 3: k1 ^= tail[2] << 16; /* fallthrough */
    case 2: k1 ^= tail[1] << 8;  /* fallthrough */
    case 1:
        k1 ^= tail[0];
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;
        h1 ^= k1;
        break;
    };

    h1 ^= len;
    h1 ^= (h1 >> 16);
    h1 *= 0x85ebca6b;
    h1 ^= (h1 >> 13);
    h1 *= 0xc2b2ae35;
    h1 ^= (h1 >> 16);

    return h1;
}

/* Used in ELF file format, also called PJW hash */
static inline uint32_t hash_elf(const void *key, size_t len) {
    const uint8_t *data = (const uint8_t *) key;
    uint32_t hash = 0;
    uint32_t x = 0;
    for (size_t i = 0; i < len; i++) {
        hash = (hash << 4) + data[i];
        if ((x = hash & 0xF0000000L) != 0) {
            hash ^= (x >> 24);
            hash &= ~x;
        }
    }
    return (hash & 0x7FFFFFFF);
}

/* Brian Kernighan and Dennis Ritchiie */
static inline uint32_t hash_bkdr(const void *key, size_t len) {
    const uint8_t *data = (const uint8_t *) key;
    uint32_t seed = 131;
    uint32_t hash = 0;
    for (size_t i = 0; i < len; i++) {
        hash = hash * seed + data[i];
    }
    return hash;
}
