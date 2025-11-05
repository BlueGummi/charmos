#include <misc/rbt.h>
#include <stdint.h>
#include <sync/spinlock.h>

#define TID_RANGE_RESERVE_COUNT 128

struct tid_range {
    struct rbt_node node;
    uint64_t start;
    uint64_t length;
    struct tid_range *next;
};

struct tid_space {
    struct rbt tree;
    struct spinlock lock;
    struct tid_range reserve_pool[TID_RANGE_RESERVE_COUNT];
    struct tid_range *reserve_free;
};

uint64_t tid_alloc(struct tid_space *ts);
void tid_free(struct tid_space *ts, uint64_t id);
struct tid_space *tid_space_init(uint64_t max_id);
