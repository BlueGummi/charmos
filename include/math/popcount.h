/* @title: Popcount */
#include <stddef.h>

static inline size_t popcount(size_t n) {
    size_t count = 0;
    while (n > 0) {
        if (n & 1)
            count++;

        n >>= 1;
    }
    return count;
}
