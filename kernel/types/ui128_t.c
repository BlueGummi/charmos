#include <console/panic.h>
#include <types/types.h>

typedef union {
    uint128_t all;
    struct {
        uint64_t lo;
        uint64_t hi;
    } s;
} u128_parts;

static inline void mul64_wide(uint64_t a, uint64_t b, uint64_t *hi,
                              uint64_t *lo) {
    uint64_t a_lo = (uint32_t) a, a_hi = a >> 32;
    uint64_t b_lo = (uint32_t) b, b_hi = b >> 32;

    uint64_t p0 = a_lo * b_lo;
    uint64_t p1 = a_lo * b_hi;
    uint64_t p2 = a_hi * b_lo;
    uint64_t p3 = a_hi * b_hi;

    uint64_t mid = (p0 >> 32) + (uint32_t) p1 + (uint32_t) p2;

    *lo = (p0 & 0xFFFFFFFFULL) | (mid << 32);
    *hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

static inline int popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int) ((x * 0x0101010101010101ULL) >> 56);
}

uint128_t __ashlti3(uint128_t a, int b) {
    u128_parts in = {.all = a}, out;

    if (b == 0)
        return a;

    if (b >= 64) {
        out.s.lo = 0;
        out.s.hi = in.s.lo << (b - 64);
    } else {
        out.s.lo = in.s.lo << b;
        out.s.hi = (in.s.hi << b) | (in.s.lo >> (64 - b));
    }
    return out.all;
}

uint128_t __lshrti3(uint128_t a, int b) {
    u128_parts in = {.all = a}, out;

    if (b == 0)
        return a;

    if (b >= 64) {
        out.s.hi = 0;
        out.s.lo = in.s.hi >> (b - 64);
    } else {
        out.s.hi = in.s.hi >> b;
        out.s.lo = (in.s.lo >> b) | (in.s.hi << (64 - b));
    }
    return out.all;
}

int128_t __ashrti3(int128_t a, int b) {
    u128_parts in = {.all = (uint128_t) a}, out;
    uint64_t fill = (uint64_t) ((int64_t) in.s.hi >> 63);

    if (b == 0)
        return a;

    if (b >= 64) {
        out.s.hi = fill;
        out.s.lo = (uint64_t) ((int64_t) in.s.hi >> (b - 64));
    } else {
        out.s.lo = (in.s.lo >> b) | (in.s.hi << (64 - b));
        out.s.hi = (uint64_t) ((int64_t) in.s.hi >> b);
    }
    return (int128_t) out.all;
}

int128_t __negti2(int128_t a) {
    u128_parts in = {.all = (uint128_t) a}, out;
    out.s.lo = ~in.s.lo + 1;
    out.s.hi = ~in.s.hi + (out.s.lo == 0 ? 1 : 0);
    return (int128_t) out.all;
}

int128_t __multi3(int128_t a, int128_t b) {
    u128_parts ap = {.all = (uint128_t) a}, bp = {.all = (uint128_t) b}, r;
    uint64_t hi, lo;

    mul64_wide(ap.s.lo, bp.s.lo, &hi, &lo);
    r.s.lo = lo;
    r.s.hi = hi + ap.s.lo * bp.s.hi + ap.s.hi * bp.s.lo;
    return (int128_t) r.all;
}

uint128_t __udivmodti4(uint128_t n, uint128_t d, uint128_t *rem) {
    u128_parts np = {.all = n}, dp = {.all = d};

    if (d == 0)
        panic("__udivmodti4: 128-bit divide by zero");

    if (np.s.hi == 0 && dp.s.hi == 0) {
        if (rem)
            *rem = np.s.lo % dp.s.lo;
        return np.s.lo / dp.s.lo;
    }

    if (n < d) {
        if (rem)
            *rem = n;
        return 0;
    }

    uint128_t q = 0, r = 0;
    int msb = (np.s.hi != 0) ? (127 - __builtin_clzll(np.s.hi))
                             : (63 - __builtin_clzll(np.s.lo));

    for (int i = msb; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (uint128_t) 1 << i;
        }
    }

    if (rem)
        *rem = r;
    return q;
}

uint128_t __udivti3(uint128_t a, uint128_t b) {
    return __udivmodti4(a, b, (uint128_t *) 0);
}

uint128_t __umodti3(uint128_t a, uint128_t b) {
    uint128_t r;
    __udivmodti4(a, b, &r);
    return r;
}

int128_t __divti3(int128_t a, int128_t b) {
    int neg = (a < 0) ^ (b < 0);
    uint128_t ua = (a < 0) ? -(uint128_t) a : (uint128_t) a;
    uint128_t ub = (b < 0) ? -(uint128_t) b : (uint128_t) b;
    uint128_t q = __udivmodti4(ua, ub, (uint128_t *) 0);
    return neg ? -(int128_t) q : (int128_t) q;
}

int128_t __modti3(int128_t a, int128_t b) {
    uint128_t ua = (a < 0) ? -(uint128_t) a : (uint128_t) a;
    uint128_t ub = (b < 0) ? -(uint128_t) b : (uint128_t) b;
    uint128_t r;
    __udivmodti4(ua, ub, &r);
    return (a < 0) ? -(int128_t) r : (int128_t) r;
}

int __cmpti2(int128_t a, int128_t b) {
    if (a < b)
        return 0;
    if (a == b)
        return 1;
    return 2;
}

int __ucmpti2(uint128_t a, uint128_t b) {
    if (a < b)
        return 0;
    if (a == b)
        return 1;
    return 2;
}

int __clzti2(uint128_t a) {
    u128_parts p = {.all = a};
    if (p.s.hi)
        return __builtin_clzll(p.s.hi);
    return 64 + __builtin_clzll(p.s.lo);
}

int __ctzti2(uint128_t a) {
    u128_parts p = {.all = a};
    if (p.s.lo)
        return __builtin_ctzll(p.s.lo);
    return 64 + __builtin_ctzll(p.s.hi);
}

int __ffsti2(int128_t a) {
    u128_parts p = {.all = (uint128_t) a};
    if (p.s.lo)
        return __builtin_ctzll(p.s.lo) + 1;
    if (p.s.hi)
        return __builtin_ctzll(p.s.hi) + 65;
    return 0;
}

int __popcountti2(uint128_t a) {
    u128_parts p = {.all = a};
    return popcount64(p.s.lo) + popcount64(p.s.hi);
}

int __parityti2(uint128_t a) {
    u128_parts p = {.all = a};
    return popcount64(p.s.lo ^ p.s.hi) & 1;
}

int128_t __negvti2(int128_t a) {
    if (a == INT128_MIN)
        panic("__negvti2: 128-bit negation overflow");
    return __negti2(a);
}

int128_t __absvti2(int128_t a) {
    if (a == INT128_MIN)
        panic("__absvti2: 128-bit abs overflow");
    return a < 0 ? __negti2(a) : a;
}

int128_t __addvti3(int128_t a, int128_t b) {
    int128_t r = (int128_t) ((uint128_t) a + (uint128_t) b);
    if (((a ^ r) & (b ^ r)) < 0)
        panic("__addvti3: 128-bit addition overflow");
    return r;
}

int128_t __subvti3(int128_t a, int128_t b) {
    int128_t r = (int128_t) ((uint128_t) a - (uint128_t) b);
    if (((a ^ b) & (a ^ r)) < 0)
        panic("__subvti3: 128-bit subtraction overflow");
    return r;
}

int128_t __mulvti3(int128_t a, int128_t b) {
    int128_t r = __multi3(a, b);
    if (a != 0 && (r / a != b || (a == -1 && b == INT128_MIN)))
        panic("__mulvti3: 128-bit multiplication overflow");
    return r;
}
