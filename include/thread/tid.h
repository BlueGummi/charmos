/* @title: Thread IDs */
#pragma once
#include <stdint.h>
#include <structures/id_space.h>

#define TID_RANGE_RESERVE_COUNT ID_RANGE_RESERVE_COUNT
#define tid_range id_range
#define tid_space id_space

static inline uint64_t tid_alloc(struct tid_space *ts) {
    return id_space_alloc(ts);
}

static inline void tid_free(struct tid_space *ts, uint64_t id) {
    id_space_free(ts, id);
}

static inline struct tid_space *tid_space_init(uint64_t max_id) {
    return id_space_init(max_id);
}
