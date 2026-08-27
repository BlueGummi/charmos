#include "../test_internal.h"

#ifdef TEST_HASH
TEST_GROUP_DECLARE(hash, .intensity_desc = {
                             .curve = SCALE_PIECEWISE_LOG,
                             .unit = "seeds",
                         });

/* We can just use existing values and published known answers, as
 * these are all published algos, so expected vals below are known answers */

struct hash_vector {
    const char *input;
    size_t len;
    uint32_t expect;
};

struct murmur_vector {
    const char *input;
    size_t len;
    uint32_t seed;
    uint32_t expect;
};

struct hash_vector64 {
    const char *input;
    size_t len;
    uint64_t expect;
};

static struct test_verdict run_vectors(const struct hash_vector *v, size_t n,
                                       uint32_t (*fn)(const void *, size_t),
                                       const char *name) {
    for (size_t i = 0; i < n; i++) {
        uint32_t got = fn(v[i].input, v[i].len);
        if (got != v[i].expect) {
            test_err("%s(\"%s\") = %08x, want %08x", name, v[i].input, got,
                     v[i].expect);
            return TEST_FAIL(name);
        }
    }
    return TEST_SUCCESS;
}

static const struct hash_vector djb2_vectors[] = {
    {"", 0, 0x00001505U},      {"a", 1, 0x0002B606U},
    {"ab", 2, 0x00597728U},    {"abc", 3, 0x0B885C8BU},
    {"abcd", 4, 0x7C93EE4FU},  {"abcde", 5, 0x0F11B894U},
    {"hello", 5, 0x0F923099U}, {"hello, world", 12, 0xB0E4250DU},
};

static const struct hash_vector sdbm_vectors[] = {
    {"", 0, 0x00000000U},      {"a", 1, 0x00000061U},
    {"ab", 2, 0x00611841U},    {"abc", 3, 0x3025F862U},
    {"abcd", 4, 0xD1BA2082U},  {"abcde", 5, 0xBD500063U},
    {"hello", 5, 0x28D19932U}, {"hello, world", 12, 0xEE6FB30CU},
};

static const struct hash_vector fnv1a_vectors[] = {
    {"", 0, 0x811C9DC5U},      {"a", 1, 0xE40C292CU},
    {"ab", 2, 0x4D2505CAU},    {"abc", 3, 0x1A47E90BU},
    {"abcd", 4, 0xCE3479BDU},  {"abcde", 5, 0x749BCF08U},
    {"hello", 5, 0x4F9F2CABU}, {"hello, world", 12, 0x4D0EA41DU},
};

static const struct hash_vector64 fnv1a_64_vectors[] = {
    {"", 0, UINT64_C(0xcbf29ce484222325)},
    {"a", 1, UINT64_C(0xaf63dc4c8601ec8c)},
    {"ab", 2, UINT64_C(0x089c4407b545986a)},
    {"abc", 3, UINT64_C(0xe71fa2190541574b)},
    {"abcd", 4, UINT64_C(0xfc179f83ee0724dd)},
    {"abcde", 5, UINT64_C(0x6348c52d762364a8)},
    {"hello", 5, UINT64_C(0xa430d84680aabd0b)},
    {"hello, world", 12, UINT64_C(0x17a1a4f267be633d)},
};

static const struct hash_vector jenkins_vectors[] = {
    {"", 0, 0x00000000U},      {"a", 1, 0xCA2E9442U},
    {"ab", 2, 0x45E61E58U},    {"abc", 3, 0xED131F5BU},
    {"abcd", 4, 0xCD8B6206U},  {"abcde", 5, 0xB98559FCU},
    {"hello", 5, 0xC8FD181BU}, {"hello, world", 12, 0x1BC6D6A4U},
};

static const struct hash_vector elf_vectors[] = {
    {"", 0, 0x00000000U},      {"a", 1, 0x00000061U},
    {"ab", 2, 0x00000672U},    {"abc", 3, 0x00006783U},
    {"abcd", 4, 0x00067894U},  {"abcde", 5, 0x006789A5U},
    {"hello", 5, 0x006EC32FU}, {"hello, world", 12, 0x08925C34U},
};

static const struct hash_vector bkdr_vectors[] = {
    {"", 0, 0x00000000U},      {"a", 1, 0x00000061U},
    {"ab", 2, 0x00003205U},    {"abc", 3, 0x001998F2U},
    {"abcd", 4, 0x0D19443AU},  {"abcde", 5, 0xB3EDEA13U},
    {"hello", 5, 0x2F372E8EU}, {"hello, world", 12, 0x81692F4CU},
};

TEST_DECLARE_UNIT(hash_known_answers, .group = TEST_GROUP(hash)) {
#define RUN(fn, vecs)                                                          \
    do {                                                                       \
        struct test_verdict v =                                                \
            run_vectors(vecs, TEST_ARRAY_LEN(vecs), fn, #fn);                  \
        if (v.result != TEST_RESULT_OK)                                        \
            return v;                                                          \
    } while (0)

    RUN(hash_djb2, djb2_vectors);
    RUN(hash_sdbm, sdbm_vectors);
    RUN(hash_fnv1a, fnv1a_vectors);
    RUN(hash_jenkins_one_at_a_time, jenkins_vectors);
    RUN(hash_elf, elf_vectors);
    RUN(hash_bkdr, bkdr_vectors);
#undef RUN

    for (size_t i = 0; i < TEST_ARRAY_LEN(fnv1a_64_vectors); i++) {
        const struct hash_vector64 *v = &fnv1a_64_vectors[i];
        TEST_ASSERT(hash_fnv1a_64(v->input, v->len) == v->expect);
    }

    uint64_t incremental = HASH_FNV1A_64_OFFSET_BASIS;
    incremental = hash_fnv1a_64_update(incremental, "hello", 5);
    incremental = hash_fnv1a_64_update(incremental, ", world", 7);
    TEST_ASSERT(incremental == hash_fnv1a_64("hello, world", 12));

    return TEST_SUCCESS;
}

static const struct murmur_vector murmur_vectors[] = {
    {"", 0, 0U, 0x00000000U},
    {"a", 1, 0U, 0x3C2569B2U},
    {"ab", 2, 0U, 0x9BBFD75FU},
    {"abc", 3, 0U, 0xB3DD93FAU},
    {"abcd", 4, 0U, 0x43ED676AU},
    {"abcde", 5, 0U, 0xE89B9AF6U},
    {"hello", 5, 0U, 0x248BFA47U},
    {"hello, world", 12, 0U, 0x149BBB7FU},
    {"", 0, 0x9747B28CU, 0xEBB6C228U},
    {"a", 1, 0x9747B28CU, 0x7FA09EA6U},
    {"ab", 2, 0x9747B28CU, 0x74875592U},
    {"abc", 3, 0x9747B28CU, 0xC84A62DDU},
    {"abcd", 4, 0x9747B28CU, 0xF0478627U},
    {"abcde", 5, 0x9747B28CU, 0xE915B832U},
    {"hello", 5, 0x9747B28CU, 0x5D7F56E8U},
    {"hello, world", 12, 0x9747B28CU, 0x9A933E00U},
};

TEST_DECLARE_UNIT(hash_murmur3_known_answers, .group = TEST_GROUP(hash)) {
    for (size_t i = 0; i < TEST_ARRAY_LEN(murmur_vectors); i++) {
        const struct murmur_vector *v = &murmur_vectors[i];
        uint32_t got = hash_murmur3_32(v->input, v->len, v->seed);
        if (got != v->expect) {
            test_err("murmur3(\"%s\", seed=%08x) = %08x, want %08x", v->input,
                     v->seed, got, v->expect);
            return TEST_FAIL("murmur3 known answer");
        }
    }
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(hash_murmur3_seed_matters, .group = TEST_GROUP(hash),
                  TEST_INTENSITY(16, 64, 4096)) {
    size_t seeds = ctx->intensity_val ? ctx->intensity_val : 64;
    const char *key = "seed sensitivity";
    size_t len = strlen(key);

    uint32_t base = hash_murmur3_32(key, len, 0);
    for (uint32_t seed = 1; seed < (uint32_t) seeds; seed++)
        TEST_ASSERT(hash_murmur3_32(key, len, seed) != base);

    return TEST_SUCCESS;
}

/* Prefix extension bugs */
TEST_DECLARE_UNIT(hash_respects_length, .group = TEST_GROUP(hash)) {
    static const char padded[] = "abcd\xFF\xFF\xFF\xFF";
    static const char clean[] = "abcd";

    TEST_ASSERT(hash_djb2(padded, 4) == hash_djb2(clean, 4));
    TEST_ASSERT(hash_sdbm(padded, 4) == hash_sdbm(clean, 4));
    TEST_ASSERT(hash_fnv1a(padded, 4) == hash_fnv1a(clean, 4));
    TEST_ASSERT(hash_jenkins_one_at_a_time(padded, 4) ==
                hash_jenkins_one_at_a_time(clean, 4));
    TEST_ASSERT(hash_elf(padded, 4) == hash_elf(clean, 4));
    TEST_ASSERT(hash_bkdr(padded, 4) == hash_bkdr(clean, 4));
    TEST_ASSERT(hash_murmur3_32(padded, 4, 0) == hash_murmur3_32(clean, 4, 0));

    for (size_t n = 1; n <= 8; n++) {
        TEST_ASSERT(hash_djb2(padded, n) != hash_djb2(padded, n - 1));
        TEST_ASSERT(hash_fnv1a(padded, n) != hash_fnv1a(padded, n - 1));
    }

    return TEST_SUCCESS;
}

/* hash_elf masks off the top bit */
TEST_DECLARE_UNIT(hash_elf_stays_31_bit, .group = TEST_GROUP(hash)) {
    uint8_t buf[16];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = 0xFF;

    for (size_t n = 0; n <= sizeof(buf); n++)
        TEST_ASSERT((hash_elf(buf, n) & 0x80000000U) == 0);

    return TEST_SUCCESS;
}

#endif
