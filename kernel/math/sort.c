/*-
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Attribution:
 * Derived from the 4.4BSD-Lite / FreeBSD C library (CSRG, UC Berkeley):
 * - lib/libc/stdlib/heapsort.c by Peter McIlroy (1992, 1993)
 * - lib/libc/stdlib/qsort.c by Jon L. Bentley and M. Douglas McIlroy (1993)
 *
 * Refactored for kernel usage:
 * - Allocation-free in-place heapsort (0 heap allocations, safe in atomic/IRQ
 * contexts)
 * - Guaranteed O(1) auxiliary stack space (non-recursive)
 * - Architecture-optimized word-aligned swap routines
 */

#include <math/sort.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define swapcode(TYPE, parmi, parmj, n)                                        \
    {                                                                          \
        size_t i = (n) / sizeof(TYPE);                                         \
        TYPE *pi = (TYPE *) (parmi);                                           \
        TYPE *pj = (TYPE *) (parmj);                                           \
        do {                                                                   \
            TYPE t = *pi;                                                      \
            *pi++ = *pj;                                                       \
            *pj++ = t;                                                         \
        } while (--i > 0);                                                     \
    }

#define SWAPINIT(a, es)                                                        \
    swaptype = ((uintptr_t) (a) % sizeof(long) || (es) % sizeof(long)) ? 2     \
               : (es) == sizeof(long)                                  ? 0     \
                                                                       : 1;

static inline void swapfunc(char *a, char *b, size_t n, size_t swaptype) {
    if (swaptype == 0) {
        long t = *(long *) (void *) (a);
        *(long *) (void *) (a) = *(long *) (void *) (b);
        *(long *) (void *) (b) = t;
    } else if (swaptype == 1) {
        swapcode(long, (void *) a, (void *) b, n);
    } else {
        swapcode(char, a, b, n);
    }
}

/*
 * Sift-down operation on a 0-indexed binary max-heap.
 * Pushes base[root] down into its correct position to restore the heap.
 */
static void sift_down(char *base, size_t root, size_t n, size_t size,
                      cmp_t *compar, size_t swaptype) {
    size_t child;
    while ((child = 2 * root + 1) < n) {
        /* If right child exists and is greater than left child, select right
         * child */
        if (child + 1 < n &&
            compar(base + (child + 1) * size, base + child * size) > 0) {
            child++;
        }

        /* If the largest child is greater than the parent, swap and descend */
        if (compar(base + child * size, base + root * size) > 0) {
            swapfunc(base + root * size, base + child * size, size, swaptype);
            root = child;
        } else {
            break;
        }
    }
}

/*
 * In-place heapsort implementation:
 * - Time complexity: O(n log n) best, average, and worst-case
 * - Auxiliary space: O(1) stack space, 0 dynamic memory allocations
 */
int heapsort(void *vbase, size_t nmemb, size_t size, cmp_t *compar) {
    char *base = (char *) vbase;
    size_t swaptype;

    if (!base || nmemb <= 1) {
        return 0;
    }

    if (size == 0) {
        return -1;
    }

    SWAPINIT(base, size);

    for (size_t i = nmemb / 2; i > 0; i--) {
        sift_down(base, i - 1, nmemb, size, compar, swaptype);
    }

    for (size_t i = nmemb - 1; i > 0; i--) {
        swapfunc(base, base + i * size, size, swaptype);
        sift_down(base, 0, i, size, compar, swaptype);
    }

    return 0;
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              cmp_t *compar) {
    if (!key || !base || size == 0) {
        return NULL;
    }

    size_t l = 0;
    size_t r = nmemb;

    while (l < r) {
        size_t mid = l + (r - l) / 2;
        const void *elem = (const char *) base + mid * size;
        int cmp = compar(key, elem);

        if (cmp < 0) {
            r = mid;
        } else if (cmp > 0) {
            l = mid + 1;
        } else {
            return (void *) elem;
        }
    }

    return NULL;
}
