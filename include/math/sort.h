/* @title: Sorting & Searching */
#pragma once
#include <stddef.h>

typedef int cmp_t(const void *, const void *);

int heapsort(void *vbase, size_t nmemb, size_t size, cmp_t *compar);
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              cmp_t *compar);
