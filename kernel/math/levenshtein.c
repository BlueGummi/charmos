#include <math/fixed.h>
#include <math/levenshtein.h>
#include <math/min_max.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline char fuzzy_fold_char(char c, bool ignore_case) {
    if (ignore_case && c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');

    return c;
}

static int64_t fuzzy_substr_distance(const char *pattern, const char *text,
                                     size_t p_len, size_t t_len,
                                     bool ignore_case) {
    int64_t *prev = valloc((t_len + 1) * sizeof(int64_t));
    int64_t *curr = valloc((t_len + 1) * sizeof(int64_t));
    int64_t best_dist = -1;

    if (!prev || !curr)
        goto out;

    for (size_t j = 0; j <= t_len; j++)
        prev[j] = 0;

    for (size_t i = 1; i <= p_len; i++) {
        curr[0] = i;

        char pc = fuzzy_fold_char(pattern[i - 1], ignore_case);

        for (size_t j = 1; j <= t_len; j++) {
            char tc = fuzzy_fold_char(text[j - 1], ignore_case);

            int64_t cost = pc == tc ? 0 : 1;

            curr[j] = MIN(curr[j - 1] + 1, prev[j] + 1, prev[j - 1] + cost);
        }

        int64_t *tmp = prev;
        prev = curr;
        curr = tmp;
    }

    best_dist = prev[0];

    for (size_t j = 1; j <= t_len; j++) {
        if (prev[j] < best_dist)
            best_dist = prev[j];
    }

out:
    vfree(prev);
    vfree(curr);

    return best_dist;
}

/* [0, 1] */
fx32_32_t fuzzy_substr_conf(const char *pattern, const char *text,
                            bool ignore_case) {
    size_t p_len = strlen(pattern);
    size_t t_len = strlen(text);

    if (p_len == 0)
        return FX_ONE;

    if (t_len == 0)
        return FX_ZERO;

    int64_t best_dist =
        fuzzy_substr_distance(pattern, text, p_len, t_len, ignore_case);

    if (best_dist < 0)
        return FX_ZERO;

    if ((size_t) best_dist >= p_len)
        return FX_ZERO;

    fx32_32_t max_len = fx_from_int(p_len);
    fx32_32_t dist = fx_div(fx_from_int(best_dist), max_len);

    return FX_ONE - dist;
}
