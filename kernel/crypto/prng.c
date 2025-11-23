#include <stdatomic.h>
#include <stdint.h>

static atomic_size_t prng_state = 88172645463325252ull;

void prng_seed(uint64_t seed) {
    if (seed != 0)
        prng_state = seed;
}

uint64_t prng_next() {
    uint64_t x = atomic_load(&prng_state);
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    atomic_store(&prng_state, x);
    return x * 0x2545F4914F6CDD1Dull;
}
