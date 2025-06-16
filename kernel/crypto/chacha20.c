#include <stdint.h>
#include <string.h>

#define ROTL32(v, n) ((v << n) | (v >> (32 - n)))

// quarter round
#define QR(a, b, c, d)                                                         \
    a += b;                                                                    \
    d ^= a;                                                                    \
    d = ROTL32(d, 16);                                                         \
    c += d;                                                                    \
    b ^= c;                                                                    \
    b = ROTL32(b, 12);                                                         \
    a += b;                                                                    \
    d ^= a;                                                                    \
    d = ROTL32(d, 8);                                                          \
    c += d;                                                                    \
    b ^= c;                                                                    \
    b = ROTL32(b, 7)

static const char *sigma = "expand 32-byte k";

static uint32_t load32_le(const uint8_t *src) {
    return ((uint32_t) src[0]) | ((uint32_t) src[1] << 8) |
           ((uint32_t) src[2] << 16) | ((uint32_t) src[3] << 24);
}

static void store32_le(uint8_t *dst, uint32_t val) {
    dst[0] = val & 0xff;
    dst[1] = (val >> 8) & 0xff;
    dst[2] = (val >> 16) & 0xff;
    dst[3] = (val >> 24) & 0xff;
}

static void chacha20_init_state(uint32_t state[16], const uint8_t key[32],
                                const uint8_t nonce[12], uint32_t counter) {
    const uint8_t *constants = (const uint8_t *) sigma;

    state[0] = load32_le(constants + 0);
    state[1] = load32_le(constants + 4);
    state[2] = load32_le(constants + 8);
    state[3] = load32_le(constants + 12);

    for (int i = 0; i < 8; i++) {
        state[4 + i] = load32_le(key + i * 4);
    }

    state[12] = counter;
    state[13] = load32_le(nonce + 0);
    state[14] = load32_le(nonce + 4);
    state[15] = load32_le(nonce + 8);
}

static void chacha20_block(uint8_t output[64], const uint32_t input[16]) {
    uint32_t x[16];
    memcpy(x, input, sizeof(x));

    for (int i = 0; i < 10; i++) {
        // Column rounds
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        // Diagonal rounds
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }

    for (int i = 0; i < 16; i++) {
        x[i] += input[i];
        store32_le(output + 4 * i, x[i]);
    }
}

// Encrypt or decrypt (it's symmetric)
void chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                      uint32_t counter, const uint8_t *in, uint8_t *out,
                      size_t len) {
    uint32_t state[16];
    uint8_t keystream[64];
    size_t offset = 0;

    while (len > 0) {
        chacha20_init_state(state, key, nonce, counter);
        chacha20_block(keystream, state);
        counter++;

        size_t block_len = len > 64 ? 64 : len;
        for (size_t i = 0; i < block_len; i++) {
            out[offset + i] = in[offset + i] ^ keystream[i];
        }

        offset += block_len;
        len -= block_len;
    }
}
