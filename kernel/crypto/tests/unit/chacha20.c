#include "crypto/tests/test_internal.h"

#ifdef TEST_CHACHA20
TEST_GROUP_DECLARE(chacha20, .intensity_desc = {
                                 .curve = SCALE_PIECEWISE_LOG,
                                 .unit = "bytes",
                             });

/* RFC 7539 Section 2.4.2 official test vector */
TEST_DECLARE_UNIT(chacha20, rfc7539_kat) {
    static const uint8_t key[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
        0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    };
    static const uint8_t nonce[12] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00,
    };
    uint32_t counter = 1;

    const char *plaintext =
        "Ladies and Gentlemen of the class of '99: If I could offer you "
        "only one tip for the future, sunscreen would be it.";
    size_t len = strlen(plaintext);

    static const uint8_t expected_cipher[114] = {
        0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28,
        0xdd, 0x0d, 0x69, 0x81, 0xe9, 0x7e, 0x7a, 0xec, 0x1d, 0x43, 0x60, 0xc2,
        0x0a, 0x27, 0xaf, 0xcc, 0xfd, 0x9f, 0xae, 0x0b, 0xf9, 0x1b, 0x65, 0xc5,
        0x52, 0x47, 0x33, 0xab, 0x8f, 0x59, 0x3d, 0xab, 0xcd, 0x62, 0xb3, 0x57,
        0x16, 0x39, 0xd6, 0x24, 0xe6, 0x51, 0x52, 0xab, 0x8f, 0x53, 0x0c, 0x35,
        0x9f, 0x08, 0x61, 0xd8, 0x07, 0xca, 0x0d, 0xbf, 0x50, 0x0d, 0x6a, 0x61,
        0x56, 0xa3, 0x8e, 0x08, 0x8a, 0x22, 0xb6, 0x5e, 0x52, 0xbc, 0x51, 0x4d,
        0x16, 0xcc, 0xf8, 0x06, 0x81, 0x8c, 0xe9, 0x1a, 0xb7, 0x79, 0x37, 0x36,
        0x5a, 0xf9, 0x0b, 0xbf, 0x74, 0xa3, 0x5b, 0xe6, 0xb4, 0x0b, 0x8e, 0xed,
        0xf2, 0x78, 0x5e, 0x42, 0x87, 0x4d,
    };

    uint8_t out[114] = {0};
    chacha20_encrypt(key, nonce, counter, (const uint8_t *) plaintext, out,
                     len);
    TEST_ASSERT_MEM_EQ(out, expected_cipher, len);

    /* D(E(M)) == M */
    uint8_t decrypted[114] = {0};
    chacha20_encrypt(key, nonce, counter, out, decrypted, len);
    TEST_ASSERT_MEM_EQ(decrypted, plaintext, len);

    return TEST_SUCCESS;
}

/* chunking and stream boundaries */
TEST_DECLARE_UNIT(chacha20, block_seams, TEST_INTENSITY(128, 512, 65536)) {
    size_t total = ctx->intensity_val ? ctx->intensity_val : 512;
    uint8_t key[32] = {0x42};
    uint8_t nonce[12] = {0x24};
    uint8_t *src = kmalloc(total, ALLOC_FLAGS_NONE);
    uint8_t *dst = kmalloc(total, ALLOC_FLAGS_NONE);
    uint8_t *roundtrip = kmalloc(total, ALLOC_FLAGS_NONE);
    TEST_ASSERT_NONNULL(src);
    TEST_ASSERT_NONNULL(dst);
    TEST_ASSERT_NONNULL(roundtrip);

    for (size_t i = 0; i < total; i++)
        src[i] = (uint8_t) i;

    /* Boundary lengths: 0, 1, 63, 64 (1 block),
     * 65 (crosses block), 128 (2 blocks), 129, total / 2, total */
    size_t lens[] = {0, 1, 63, 64, 65, 128, 129, total / 2, total};
    for (size_t i = 0; i < TEST_ARRAY_LEN(lens); i++) {
        size_t l = lens[i];
        if (l > total)
            l = total;
        memset(dst, 0, total);
        memset(roundtrip, 0, total);

        chacha20_encrypt(key, nonce, 1, src, dst, l);
        chacha20_encrypt(key, nonce, 1, dst, roundtrip, l);

        TEST_ASSERT_MEM_EQ(roundtrip, src, l);
    }

    kfree(src);
    kfree(dst);
    kfree(roundtrip);
    return TEST_SUCCESS;
}
#endif
