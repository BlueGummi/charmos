#pragma once
#include <stddef.h>
#include <stdint.h>

void chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                      uint32_t counter, const uint8_t *in, uint8_t *out,
                      size_t len);
