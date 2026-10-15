#include <atomic.h>
#include <types/types.h>

/* GCC references these when expecting libatomic, and we don't
 * bother with the memory ordering constraint since cmpxchg16b is
 * a full barrier anyways */
static inline bool cmpxchg16b(volatile void *ptr, uint128_t *expected,
                              uint128_t desired) {
    uint64_t lo = (uint64_t) *expected;
    uint64_t hi = (uint64_t) (*expected >> 64);
    bool ok;

    asm volatile("lock cmpxchg16b %[mem]"
                 : [mem] "+m"(*(volatile uint128_t *) ptr), "+a"(lo), "+d"(hi),
                   "=@ccz"(ok)
                 : "b"((uint64_t) desired), "c"((uint64_t) (desired >> 64))
                 : "memory");

    *expected = ((uint128_t) hi << 64) | lo;
    return ok;
}

uint128_t __atomic_load_16(const volatile void *ptr, int mo) {
    cc_ignore(mo);
    uint128_t cur = 0;
    cmpxchg16b((volatile void *) ptr, &cur, 0);
    return cur;
}

void __atomic_store_16(volatile void *ptr, uint128_t val, int mo) {
    cc_ignore(mo);
    uint128_t cur = 0;
    while (!cmpxchg16b(ptr, &cur, val))
        ;
}

uint128_t __atomic_exchange_16(volatile void *ptr, uint128_t val, int mo) {
    cc_ignore(mo);
    uint128_t cur = 0;
    while (!cmpxchg16b(ptr, &cur, val))
        ;
    return cur;
}

bool atomic_compare_exchange_16(
    volatile void *ptr, void *expected, uint128_t desired, int success,
    int failure) asm("__atomic_compare_exchange_16");

bool atomic_compare_exchange_16(volatile void *ptr, void *expected,
                                uint128_t desired, int success, int failure) {
    cc_ignore(success, failure);
    return cmpxchg16b(ptr, expected, desired);
}
