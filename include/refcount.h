#include <stdatomic.h>
#include <stdbool.h>
#include <types.h>

static inline void refcount_init(refcount_t *rc, unsigned int val) {
    atomic_store(rc, val);
}

static inline void refcount_inc(refcount_t *rc) {
    atomic_fetch_add(rc, 1);
}

static inline unsigned int refcount_dec(refcount_t *rc) {
    return atomic_fetch_sub(rc, 1) - 1;
}

static inline unsigned int refcount_read(refcount_t *rc) {
    return atomic_load(rc);
}

static inline bool refcount_dec_and_test(refcount_t *rc) {
    return atomic_fetch_sub(rc, 1) == 1;
}
