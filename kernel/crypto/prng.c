#include <smp/core.h>
#include <smp/percpu.h>
#include <stdint.h>
#include <string.h>

struct prng_core {
    uint32_t state[16];
    uint8_t buffer[64]; // keystream buffer
    size_t pos;
};

void prng_build(struct prng_core *this_one, size_t cpu) {}

PERCPU_DECLARE(pcs, struct prng_core, prng_build);
#define prng_core_state PERCPU_READ(pcs)

static void prng_seed_core(uint64_t seed) {
    const char *sigma = "expand 32-byte k";
    uint8_t key[32];

    uint64_t tsc = smp_core()->last_tsc;
    uint32_t core_id = smp_core()->id;

    for (int i = 0; i < 4; i++) {
        key[i * 8 + 0] = (seed >> (i * 16)) & 0xff;
        key[i * 8 + 1] = (seed >> (i * 16 + 8)) & 0xff;
        key[i * 8 + 2] = (tsc >> (i * 16)) & 0xff;
        key[i * 8 + 3] = (tsc >> (i * 16 + 8)) & 0xff;
        key[i * 8 + 4] = (core_id >> (i * 8)) & 0xff;
        key[i * 8 + 5] = (core_id >> (i * 8 + 4)) & 0xff;
        key[i * 8 + 6] = (tsc >> (i * 16 + 32)) & 0xff;
        key[i * 8 + 7] = (tsc >> (i * 16 + 40)) & 0xff;
    }

    for (int i = 0; i < 4; i++)
        prng_core_state.state[i] = ((uint32_t *) sigma)[i];
    for (int i = 0; i < 8; i++)
        prng_core_state.state[4 + i] = ((uint32_t *) key)[i];

    prng_core_state.state[12] = 0;
    prng_core_state.state[13] = 0;
    prng_core_state.state[14] = 0;
    prng_core_state.state[15] = 0;

    prng_core_state.pos = 64;
}

#define QR(a, b, c, d)                                                         \
    a += b;                                                                    \
    d ^= a;                                                                    \
    d = (d << 16) | (d >> 16);                                                 \
    c += d;                                                                    \
    b ^= c;                                                                    \
    b = (b << 12) | (b >> 20);                                                 \
    a += b;                                                                    \
    d ^= a;                                                                    \
    d = (d << 8) | (d >> 24);                                                  \
    c += d;                                                                    \
    b ^= c;                                                                    \
    b = (b << 7) | (b >> 25)

static void chacha20_generate_block(uint8_t out[64], uint32_t state[16]) {
    uint32_t x[16];
    memcpy(x, state, sizeof(x));

    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }

    for (int i = 0; i < 16; i++) {
        x[i] += state[i];
        out[i * 4 + 0] = x[i] & 0xff;
        out[i * 4 + 1] = (x[i] >> 8) & 0xff;
        out[i * 4 + 2] = (x[i] >> 16) & 0xff;
        out[i * 4 + 3] = (x[i] >> 24) & 0xff;
    }
}

uint64_t prng_next(void) {
    if (prng_core_state.pos >= 64) {
        chacha20_generate_block(prng_core_state.buffer, prng_core_state.state);
        prng_core_state.state[12]++;
        prng_core_state.pos = 0;
    }

    uint64_t val;
    memcpy(&val, prng_core_state.buffer + prng_core_state.pos,
           sizeof(uint64_t));
    prng_core_state.pos += 8;
    return val;
}

void prng_seed(uint64_t seed) {
    if (seed == 0)
        seed = smp_core()->last_tsc;
    prng_seed_core(seed);
}
