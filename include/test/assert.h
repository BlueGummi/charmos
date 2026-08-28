/* @title: Test Assertion Macros */
#pragma once
#include <compiler.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* We expect
 * - test_err(fmt, ...)
 * - TEST_FAIL(msg)
 * - test_global
 *
 * from test.h
 */

/* ==================== Boolean + Truthiness ==================== */

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            test_err("assert \"%s\" failed", #cond);                           \
            return TEST_FAIL(#cond);                                           \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_TRUE(cond) TEST_ASSERT(cond)

#define TEST_ASSERT_FALSE(cond)                                                \
    do {                                                                       \
        if (cond) {                                                            \
            test_err("assert false \"%s\" failed (was true)", #cond);          \
            return TEST_FAIL("!" #cond);                                       \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_MSG(cond, fmt, ...)                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            test_err("assert \"%s\" failed: " fmt, #cond, ##__VA_ARGS__);      \
            return TEST_FAIL(#cond);                                           \
        }                                                                      \
    } while (0)

/* ==================== Pointer ==================== */
#define TEST_ASSERT_NULL(ptr)                                                  \
    do {                                                                       \
        const void *_p = (const void *) (ptr);                                 \
        if (_p != NULL) {                                                      \
            test_err("assert \"%s == NULL\" failed (got %p)", #ptr, _p);       \
            return TEST_FAIL(#ptr " == NULL");                                 \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_NONNULL(ptr)                                               \
    do {                                                                       \
        const void *_p = (const void *) (ptr);                                 \
        if (_p == NULL) {                                                      \
            test_err("assert \"%s != NULL\" failed (got NULL)", #ptr);         \
            return TEST_FAIL(#ptr " != NULL");                                 \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_PTR_EQ(a, b)                                               \
    do {                                                                       \
        const void *_pa = (const void *) (a);                                  \
        const void *_pb = (const void *) (b);                                  \
        if (_pa != _pb) {                                                      \
            test_err("assert ptr \"%s == %s\" failed (%p != %p)", #a, #b, _pa, \
                     _pb);                                                     \
            return TEST_FAIL(#a " == " #b);                                    \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_PTR_NE(a, b)                                               \
    do {                                                                       \
        const void *_pa = (const void *) (a);                                  \
        const void *_pb = (const void *) (b);                                  \
        if (_pa == _pb) {                                                      \
            test_err("assert ptr \"%s != %s\" failed (both %p)", #a, #b, _pa); \
            return TEST_FAIL(#a " != " #b);                                    \
        }                                                                      \
    } while (0)

/* ====================  Unsigned Equality & Comparison ==================== */
#define TEST_ASSERT_EQ(a, b)                                                   \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if ((uint64_t) (_a) != (uint64_t) (_b)) {                              \
            test_err("assert \"%s == %s\" failed (%llu != %llu / 0x%llx != "   \
                     "0x%llx)",                                                \
                     #a, #b, (unsigned long long) (uint64_t) (_a),             \
                     (unsigned long long) (uint64_t) (_b),                     \
                     (unsigned long long) (uint64_t) (_a),                     \
                     (unsigned long long) (uint64_t) (_b));                    \
            return TEST_FAIL(#a " == " #b);                                    \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_NE(a, b)                                                   \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if ((uint64_t) (_a) == (uint64_t) (_b)) {                              \
            test_err("assert \"%s != %s\" failed (both equal %llu / 0x%llx)",  \
                     #a, #b, (unsigned long long) (uint64_t) (_a),             \
                     (unsigned long long) (uint64_t) (_a));                    \
            return TEST_FAIL(#a " != " #b);                                    \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_LT(a, b)                                                   \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if (!((uint64_t) (_a) < (uint64_t) (_b))) {                            \
            test_err("assert \"%s < %s\" failed (%llu >= %llu)", #a, #b,       \
                     (unsigned long long) (uint64_t) (_a),                     \
                     (unsigned long long) (uint64_t) (_b));                    \
            return TEST_FAIL(#a " < " #b);                                     \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_LE(a, b)                                                   \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if (!((uint64_t) (_a) <= (uint64_t) (_b))) {                           \
            test_err("assert \"%s <= %s\" failed (%llu > %llu)", #a, #b,       \
                     (unsigned long long) (uint64_t) (_a),                     \
                     (unsigned long long) (uint64_t) (_b));                    \
            return TEST_FAIL(#a " <= " #b);                                    \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_GT(a, b)                                                   \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if (!((uint64_t) (_a) > (uint64_t) (_b))) {                            \
            test_err("assert \"%s > %s\" failed (%llu <= %llu)", #a, #b,       \
                     (unsigned long long) (uint64_t) (_a),                     \
                     (unsigned long long) (uint64_t) (_b));                    \
            return TEST_FAIL(#a " > " #b);                                     \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_GE(a, b)                                                   \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if (!((uint64_t) (_a) >= (uint64_t) (_b))) {                           \
            test_err("assert \"%s >= %s\" failed (%llu < %llu)", #a, #b,       \
                     (unsigned long long) (uint64_t) (_a),                     \
                     (unsigned long long) (uint64_t) (_b));                    \
            return TEST_FAIL(#a " >= " #b);                                    \
        }                                                                      \
    } while (0)

/* ==================== Signed Integer Comparisons ==================== */
#define TEST_ASSERT_EQ_S(a, b)                                                 \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if ((int64_t) (_a) != (int64_t) (_b)) {                                \
            test_err("assert \"%s == %s\" failed (%lld != %lld)", #a, #b,      \
                     (long long) (int64_t) (_a), (long long) (int64_t) (_b));  \
            return TEST_FAIL(#a " == " #b);                                    \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_LT_S(a, b)                                                 \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if (!((int64_t) (_a) < (int64_t) (_b))) {                              \
            test_err("assert \"%s < %s\" failed (%lld >= %lld)", #a, #b,       \
                     (long long) (int64_t) (_a), (long long) (int64_t) (_b));  \
            return TEST_FAIL(#a " < " #b);                                     \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_LE_S(a, b)                                                 \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if (!((int64_t) (_a) <= (int64_t) (_b))) {                             \
            test_err("assert \"%s <= %s\" failed (%lld > %lld)", #a, #b,       \
                     (long long) (int64_t) (_a), (long long) (int64_t) (_b));  \
            return TEST_FAIL(#a " <= " #b);                                    \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_GT_S(a, b)                                                 \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if (!((int64_t) (_a) > (int64_t) (_b))) {                              \
            test_err("assert \"%s > %s\" failed (%lld <= %lld)", #a, #b,       \
                     (long long) (int64_t) (_a), (long long) (int64_t) (_b));  \
            return TEST_FAIL(#a " > " #b);                                     \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_GE_S(a, b)                                                 \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if (!((int64_t) (_a) >= (int64_t) (_b))) {                             \
            test_err("assert \"%s >= %s\" failed (%lld < %lld)", #a, #b,       \
                     (long long) (int64_t) (_a), (long long) (int64_t) (_b));  \
            return TEST_FAIL(#a " >= " #b);                                    \
        }                                                                      \
    } while (0)

/* ====================  Memory and String ==================== */
#define TEST_ASSERT_STR_EQ(a, b)                                               \
    do {                                                                       \
        const char *_sa = (const char *) (a);                                  \
        const char *_sb = (const char *) (b);                                  \
        if (_sa == NULL || _sb == NULL || strcmp(_sa, _sb) != 0) {             \
            test_err("assert str \"%s == %s\" failed (\"%s\" != \"%s\")", #a,  \
                     #b, _sa ? _sa : "<NULL>", _sb ? _sb : "<NULL>");          \
            return TEST_FAIL(#a " == " #b);                                    \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_STR_NE(a, b)                                               \
    do {                                                                       \
        const char *_sa = (const char *) (a);                                  \
        const char *_sb = (const char *) (b);                                  \
        if (_sa == _sb ||                                                      \
            (_sa != NULL && _sb != NULL && strcmp(_sa, _sb) == 0)) {           \
            test_err("assert str \"%s != %s\" failed (both \"%s\")", #a, #b,   \
                     _sa ? _sa : "<NULL>");                                    \
            return TEST_FAIL(#a " != " #b);                                    \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_MEM_EQ(a, b, size)                                         \
    do {                                                                       \
        const void *_ma = (const void *) (a);                                  \
        const void *_mb = (const void *) (b);                                  \
        size_t _sz = (size_t) (size);                                          \
        if (memcmp(_ma, _mb, _sz) != 0) {                                      \
            test_err("assert mem \"%s == %s\" failed (size %zu)", #a, #b,      \
                     _sz);                                                     \
            return TEST_FAIL(#a " == " #b);                                    \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_MEM_ZERO(ptr, size)                                        \
    do {                                                                       \
        const uint8_t *_pz = (const uint8_t *) (ptr);                          \
        size_t _sz = (size_t) (size);                                          \
        bool _all_zero = true;                                                 \
        size_t _first_bad = 0;                                                 \
        for (size_t _i = 0; _i < _sz; _i++) {                                  \
            if (_pz[_i] != 0) {                                                \
                _all_zero = false;                                             \
                _first_bad = _i;                                               \
                break;                                                         \
            }                                                                  \
        }                                                                      \
        if (!_all_zero) {                                                      \
            test_err("assert mem_zero \"%s\" failed (nonzero byte 0x%02x at "  \
                     "offset %zu of %zu)",                                     \
                     #ptr, _pz[_first_bad], _first_bad, _sz);                  \
            return TEST_FAIL(#ptr " is zero");                                 \
        }                                                                      \
    } while (0)

/* ====================  Errors and statuses ==================== */
#define TEST_ASSERT_OK(err)                                                    \
    do {                                                                       \
        __typeof__(err) _err = (err);                                          \
        if (_err != 0) {                                                       \
            test_err("assert ok \"%s == 0\" failed (error code %lld / "        \
                     "0x%llx)",                                                \
                     #err, (long long) (int64_t) (_err),                       \
                     (unsigned long long) (uint64_t) (_err));                  \
            return TEST_FAIL(#err " == 0");                                    \
        }                                                                      \
    } while (0)

/* ====================  Range and Bit Manipulation ==================== */
#define TEST_ASSERT_IN_RANGE(val, min, max)                                    \
    do {                                                                       \
        __typeof__(val) _v = (val);                                            \
        __typeof__(min) _min = (min);                                          \
        __typeof__(max) _max = (max);                                          \
        if (!((_v) >= (_min) && (_v) <= (_max))) {                             \
            test_err("assert in range \"%s <= %s <= %s\" failed (val=%llu, "   \
                     "range=[%llu, %llu])",                                    \
                     #min, #val, #max, (unsigned long long) (uint64_t) (_v),   \
                     (unsigned long long) (uint64_t) (_min),                   \
                     (unsigned long long) (uint64_t) (_max));                  \
            return TEST_FAIL(#val " in range [" #min ", " #max "]");           \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_BIT_SET(val, bit)                                          \
    do {                                                                       \
        uint64_t _v = (uint64_t) (val);                                        \
        uint32_t _b = (uint32_t) (bit);                                        \
        if (!(_v & (1ULL << _b))) {                                            \
            test_err("assert bit set \"%s bit %u\" failed (val=0x%llx)", #val, \
                     _b, (unsigned long long) _v);                             \
            return TEST_FAIL(#val " bit " #bit " is set");                     \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_BIT_CLEAR(val, bit)                                        \
    do {                                                                       \
        uint64_t _v = (uint64_t) (val);                                        \
        uint32_t _b = (uint32_t) (bit);                                        \
        if (_v & (1ULL << _b)) {                                               \
            test_err("assert bit clear \"%s bit %u\" failed (val=0x%llx)",     \
                     #val, _b, (unsigned long long) _v);                       \
            return TEST_FAIL(#val " bit " #bit " is clear");                   \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_MASK_SET(val, mask)                                        \
    do {                                                                       \
        uint64_t _v = (uint64_t) (val);                                        \
        uint64_t _m = (uint64_t) (mask);                                       \
        if ((_v & _m) != _m) {                                                 \
            test_err("assert mask set \"%s & %s == %s\" failed (val=0x%llx, "  \
                     "mask=0x%llx)",                                           \
                     #val, #mask, #mask, (unsigned long long) _v,              \
                     (unsigned long long) _m);                                 \
            return TEST_FAIL(#val " has mask " #mask);                         \
        }                                                                      \
    } while (0)

/* ====================  For void helper functions ==================== */
#define TEST_ASSERT_VOID(cond)                                                 \
    do {                                                                       \
        if (!(cond)) {                                                         \
            test_err("assert void \"%s\" failed", #cond);                      \
            return;                                                            \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_VOID_MSG(cond, fmt, ...)                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            test_err("assert void \"%s\" failed: " fmt, #cond, ##__VA_ARGS__); \
            return;                                                            \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_VOID_EQ(a, b)                                              \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if ((uint64_t) (_a) != (uint64_t) (_b)) {                              \
            test_err("assert void \"%s == %s\" failed (%llu != %llu / 0x%llx " \
                     "!= 0x%llx)",                                             \
                     #a, #b, (unsigned long long) (uint64_t) (_a),             \
                     (unsigned long long) (uint64_t) (_b),                     \
                     (unsigned long long) (uint64_t) (_a),                     \
                     (unsigned long long) (uint64_t) (_b));                    \
            return;                                                            \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_VOID_NONNULL(ptr)                                          \
    do {                                                                       \
        const void *_p = (const void *) (ptr);                                 \
        if (_p == NULL) {                                                      \
            test_err("assert void \"%s != NULL\" failed", #ptr);               \
            return;                                                            \
        }                                                                      \
    } while (0)

/* ====================  Soft assertions ==================== */
#define TEST_EXPECT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            test_err("expect \"%s\" failed", #cond);                           \
            if (test_global.current_test)                                      \
                test_global.current_test->soft_fails++;                        \
        }                                                                      \
    } while (0)

#define TEST_EXPECT_EQ(a, b)                                                   \
    do {                                                                       \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        if ((uint64_t) (_a) != (uint64_t) (_b)) {                              \
            test_err("expect \"%s == %s\" failed (%llu != %llu / 0x%llx != "   \
                     "0x%llx)",                                                \
                     #a, #b, (unsigned long long) (uint64_t) (_a),             \
                     (unsigned long long) (uint64_t) (_b),                     \
                     (unsigned long long) (uint64_t) (_a),                     \
                     (unsigned long long) (uint64_t) (_b));                    \
            if (test_global.current_test)                                      \
                test_global.current_test->soft_fails++;                        \
        }                                                                      \
    } while (0)

#define TEST_EXPECT_NONNULL(ptr)                                               \
    do {                                                                       \
        const void *_p = (const void *) (ptr);                                 \
        if (_p == NULL) {                                                      \
            test_err("expect \"%s != NULL\" failed", #ptr);                    \
            if (test_global.current_test)                                      \
                test_global.current_test->soft_fails++;                        \
        }                                                                      \
    } while (0)

#define TEST_EXPECT_MSG(cond, fmt, ...)                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            test_err("expect \"%s\" failed: " fmt, #cond, ##__VA_ARGS__);      \
            if (test_global.current_test)                                      \
                test_global.current_test->soft_fails++;                        \
        }                                                                      \
    } while (0)
