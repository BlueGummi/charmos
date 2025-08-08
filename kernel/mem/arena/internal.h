#include <mem/arena.h>

static inline bool arena_lock(struct arena *a) {
    return spin_lock(&a->lock);
}

static inline void arena_unlock(struct arena *a, bool iflag) {
    return spin_unlock(&a->lock, iflag);
}
