// Taken from
// https://github.com/menix-os/menix/blob/legacy/include/menix/util/hash_map.h
//
// Thank you Menix!

#pragma once
#include <stdint.h>

// Maximum length of key data.
#define HASHMAP_KEY_LEN 256

// Defines a hash map with a string as key and `type` as value type.
#define hash_map(type)                                                         \
    struct {                                                                   \
        struct {                                                               \
            struct {                                                           \
                type item;                                                     \
                uint8_t key_data[HASHMAP_KEY_LEN];                             \
                uint64_t key_len;                                              \
            } *items;                                                          \
            uint64_t capacity;                                                 \
            uint64_t count;                                                    \
        } *buckets;                                                            \
        uint64_t capacity;                                                     \
    }

static inline uint32_t hash(const void *data, uint64_t length) {
    const uint8_t *data_u8 = data;
    uint32_t hash = 0;

    for (uint64_t i = 0; i < length; i++) {
        uint32_t c = data_u8[i];
        hash = c + (hash << 6) + (hash << 16) - hash;
    }

    return hash;
}

// Initializes a hashmap `map` with a capacity of `cap` elements.
#define hashmap_init(map, cap)                                                 \
    map = (typeof(map)) {                                                      \
        .capacity = cap, .buckets = NULL,                                      \
    }

// Inserts `value` with a connected `key` of `key_length` length into `map`.
#define hashmap_insert(map, key, key_length, value)                            \
    do {                                                                       \
        /* Copy macro values over. */                                          \
        auto __key = key;                                                      \
        auto __key_len = key_length;                                           \
        auto __map = map;                                                      \
        /* Allocate buckets */                                                 \
        if (__map->buckets == NULL)                                            \
            __map->buckets =                                                   \
                kzalloc(__map->capacity * sizeof(*(__map->buckets)));          \
        uint64_t __hash = hash(__key, __key_len);                              \
        uint64_t __index = __hash % __map->capacity;                           \
        auto __bucket = &__map->buckets[__index];                              \
        /* Allocate items for current bucket. */                               \
        if (__bucket->capacity == 0) {                                         \
            __bucket->capacity = 16;                                           \
            __bucket->items =                                                  \
                kzalloc(__bucket->capacity * sizeof(*__bucket->items));        \
        }                                                                      \
        if (__bucket->count == __bucket->capacity) {                           \
            __bucket->capacity *= 2;                                           \
            __bucket->items =                                                  \
                krealloc(__bucket->items,                                      \
                         __bucket->capacity * sizeof(*__bucket->items));       \
        }                                                                      \
        auto __item = &__bucket->items[__bucket->count];                       \
        memcpy(&__item->key_data[0], __key, __key_len);                        \
        __item->key_len = __key_len;                                           \
        __item->item = (value);                                                \
        __bucket->count++;                                                     \
    } while (0)

#define hashmap_remove(map, key, key_length)                                   \
    ({                                                                         \
        __label__ __stop;                                                      \
        bool __ok = false;                                                     \
        auto __key_data = (key);                                               \
        auto __key_len = (key_length);                                         \
        auto __hashmap = (map);                                                \
        if (__hashmap->buckets == NULL)                                        \
            goto __stop;                                                       \
        uint64_t __hash = hash(__key_data, __key_len);                         \
        uint64_t __index = __hash % __hashmap->capacity;                       \
        auto __bucket = &__hashmap->buckets[__index];                          \
        for (uint64_t __i = 0; __i < __bucket->count; __i++) {                 \
            if (__key_len != __bucket->items[__i].key_len)                     \
                continue;                                                      \
            if (memcmp(__key_data, __bucket->items[__i].key_data,              \
                       __key_len) == 0) {                                      \
                if (__i != __bucket->count - 1) {                              \
                    memcpy(&__bucket->items[__i],                              \
                           &__bucket->items[__bucket->count - 1],              \
                           sizeof(*__bucket->items));                          \
                }                                                              \
                __bucket->count -= 1;                                          \
                __ok = true;                                                   \
                break;                                                         \
            }                                                                  \
        }                                                                      \
    __stop:                                                                    \
        __ok;                                                                  \
    })

#define hashmap_get(map, ret, key, key_length)                                 \
    ({                                                                         \
        __label__ __stop;                                                      \
        bool __ok = false;                                                     \
        auto __key_data = (key);                                               \
        auto __key_len = (key_length);                                         \
        auto __map = (map);                                                    \
        if (__map->buckets == NULL)                                            \
            goto __stop;                                                       \
        uint64_t __hash = hash(__key_data, __key_len);                         \
        uint64_t __index = __hash % __map->capacity;                           \
        auto __bucket = &__map->buckets[__index];                              \
        for (uint64_t __i = 0; __i < __bucket->count; __i++) {                 \
            if (__key_len != __bucket->items[__i].key_len)                     \
                continue;                                                      \
            if (memcmp(__key_data, __bucket->items[__i].key_data,              \
                       __key_len) == 0) {                                      \
                (ret) = __bucket->items[__i].item;                             \
                __ok = true;                                                   \
                break;                                                         \
            }                                                                  \
        }                                                                      \
    __stop:                                                                    \
        __ok;                                                                  \
    })
