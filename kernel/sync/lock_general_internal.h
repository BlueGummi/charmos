#include <asm.h>
#include <crypto/prng.h>
#include <stddef.h>

/* jitter to avoid lockstep */
static inline int32_t backoff_jitter(size_t backoff, size_t pct) {
    uint32_t v = (uint32_t) prng_next();
    int32_t denom = (int32_t) (backoff * pct / 100);

    if (denom <= 0)
        denom = 1;

    return (int32_t) (v % (uint32_t) denom);
}

static inline void lock_delay(size_t backoff, size_t pct) {
    /* give it jitter so we don't all spin for
     * precisely the same amount of cycles */
    int32_t jitter = backoff_jitter(backoff, pct);

    if ((int64_t) backoff + (int64_t) jitter < 0)
        jitter = 0; /* no jitter, we are underflowing */

    backoff += jitter;

    for (size_t i = 0; i < backoff; i++)
        cpu_relax();
}
