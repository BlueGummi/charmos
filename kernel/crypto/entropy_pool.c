#include <crypto/chacha20.h>
#include <crypto/entropy_pool.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "spin_lock.h"

static uint8_t chacha_key[32] = {0};
static uint8_t chacha_nonce[12] = {0};
static uint32_t chacha_counter = 0;
static uint64_t chacha_reseed_counter = 0;

void entropy_pool_init(struct entropy_pool *pool) {
    memset(pool->buffer, 0, ENTROPY_POOL_SIZE);
    pool->write_pos = 0;
    pool->entropy_bits = 0;
    memset(&pool->lock, 0, sizeof(struct spinlock));
}

void entropy_pool_add(struct entropy_pool *pool, const uint8_t *data,
                      uint64_t len, uint64_t entropy_bits) {
    bool ints = spin_lock(&pool->lock);

    for (uint64_t i = 0; i < len; i++) {
        pool->buffer[pool->write_pos] ^= data[i];
        pool->write_pos = (pool->write_pos + 1) % ENTROPY_POOL_SIZE;
    }

    pool->entropy_bits += entropy_bits;
    if (pool->entropy_bits > ENTROPY_MAX_BITS)
        pool->entropy_bits = ENTROPY_MAX_BITS;

    spin_unlock(&pool->lock, ints);
}

size_t entropy_pool_extract(struct entropy_pool *pool, uint8_t *out,
                            size_t len) {
    uint8_t seed[32];

    bool ints = spin_lock(&pool->lock);

    // Refuse to output if there's insufficient entropy
    if (pool->entropy_bits < len * 8) {
        spin_unlock(&pool->lock, ints);
        return 0;
    }

    memcpy(seed, pool->buffer, 32);

    // Reduce entropy count
    if (pool->entropy_bits >= len * 8)
        pool->entropy_bits -= len * 8;
    else
        pool->entropy_bits = 0;

    spin_unlock(&pool->lock, ints);

    chacha20_encrypt(seed, chacha_nonce, 0, (uint8_t[64]) {0}, out, len);
    return len;
}

size_t entropy_pool_bits(struct entropy_pool *pool) {
    bool ints = spin_lock(&pool->lock);
    size_t bits = pool->entropy_bits;
    spin_unlock(&pool->lock, ints);
    return bits;
}

void entropy_pool_decrease(struct entropy_pool *pool, size_t bits) {
    bool ints = spin_lock(&pool->lock);
    if (pool->entropy_bits > bits)
        pool->entropy_bits -= bits;
    else
        pool->entropy_bits = 0;
    spin_unlock(&pool->lock, ints);
}

void entropy_pool_seed(struct entropy_pool *pool) {
    size_t bits = entropy_pool_bits(pool);
    if (bits < 256) {
        // Wait for more entropy
        return;
    }

    entropy_pool_extract(pool, chacha_key, 32);
    memset(chacha_nonce, 0, sizeof(chacha_nonce));
    chacha_counter = 0;
}

void entropy_pool_reseed(struct entropy_pool *pool) {
    if (entropy_pool_bits(pool) >= 128 && chacha_reseed_counter++ > 10000) {
        uint8_t seed[32];
        entropy_pool_extract(pool, seed, 32);
        for (int i = 0; i < 32; i++) {
            chacha_key[i] ^= seed[i];
        }
        chacha_reseed_counter = 0;
    }
}
