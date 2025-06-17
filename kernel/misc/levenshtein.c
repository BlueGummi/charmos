#include <mem/alloc.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

int64_t levenshtein_distance(const char *s1, const char *s2) {
    int64_t len1 = strlen(s1);
    int64_t len2 = strlen(s2);

    int64_t *prev = kmalloc((len2 + 1) * sizeof(int64_t));
    int64_t *curr = kmalloc((len2 + 1) * sizeof(int64_t));
    if (!prev || !curr)
        return -1;

    for (int64_t j = 0; j <= len2; j++)
        prev[j] = j;

    for (int64_t i = 1; i <= len1; i++) {
        curr[0] = i;
        for (int64_t j = 1; j <= len2; j++) {
            int64_t cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
            curr[j] =
                MIN(MIN(curr[j - 1] + 1, prev[j] + 1), prev[j - 1] + cost);
        }
        int64_t *tmp = prev;
        prev = curr;
        curr = tmp;
    }

    int64_t result = prev[len2];
    kfree(prev);
    kfree(curr);
    return result;
}
