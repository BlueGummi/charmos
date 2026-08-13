#include <kassert.h>
#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <parse.h>
#include <string.h>

bool parse_bool(const char *str) {
    kassert(str);

    size_t len = strlen(str);
    char *lower_str = kmalloc_or_die(len + 1);

    for (size_t i = 0; i < len; i++)
        lower_str[i] = tolower((unsigned char) str[i]);

    lower_str[len] = '\0';

    const char *enabled_terms[] = {
        "true",     "enabled", "y",      "yes",   "yeah", "yup", "on",
        "positive", "1",       "active", "allow", "ok",   "open"};

    int num_enabled = sizeof(enabled_terms) / sizeof(enabled_terms[0]);

    const char *disabled_terms[] = {
        "false",    "disabled", "n",        "no",   "nope",    "off",
        "negative", "0",        "inactive", "deny", "blocked", "closed"};
    int num_disabled = sizeof(disabled_terms) / sizeof(disabled_terms[0]);

    for (int i = 0; i < num_enabled; i++) {
        if (strcmp(lower_str, enabled_terms[i]) == 0) {
            kfree(lower_str);
            return true;
        }
    }

    for (int i = 0; i < num_disabled; i++) {
        if (strcmp(lower_str, disabled_terms[i]) == 0) {
            kfree(lower_str);
            return false;
        }
    }

    panic("invalid value '%s'", str);
}

ssize_t parse_csv(const char *input, char ***output) {
    if (!input || !output)
        return -1;

    size_t len = strlen(input);

    bool all_spaces = true;
    size_t last_char_idx = 0;
    for (size_t i = len; i > 0; i--) {
        if (!isspace((unsigned char) input[i - 1])) {
            all_spaces = false;
            last_char_idx = i - 1;
            break;
        }
    }

    if (all_spaces) {
        *output = NULL;
        return 0;
    }

    if (input[last_char_idx] == ',')
        return -1;

    /* Array size, ssize_t since this is what we return */
    ssize_t count = 1;
    for (size_t i = 0; i <= last_char_idx; i++) {
        if (input[i] == ',') {
            count++;
        }
    }

    char **arr = kmalloc(count * sizeof(char *));
    if (!arr)
        return -1;

    const char *start = input;
    ssize_t idx = 0;

    /* We need to parse, trim, and then copy each of these into its
     * own string inside the char ***output */
    while (*start != '\0' && idx < count) {
        const char *end = strchr(start, ',');
        if (!end)
            end = start + strlen(start);

        const char *val_start = start;
        while (val_start < end && isspace((unsigned char) *val_start))
            val_start++;

        const char *val_end = end;
        while (val_end > val_start && isspace((unsigned char) *(val_end - 1)))
            val_end--;

        /* Trimmed value */
        size_t val_len = val_end - val_start;
        arr[idx] = kmalloc(val_len + 1);
        if (!arr[idx]) {
            for (ssize_t k = 0; k < idx; k++)
                kfree(arr[k]);

            kfree(arr);
            return -1;
        }

        memcpy(arr[idx], val_start, val_len);
        arr[idx][val_len] = '\0';
        idx++;

        if (*end == '\0')
            break;

        start = end + 1; /* Next iteration/value */
    }

    *output = arr;
    return count;
}

ssize_t parse_data_size(const char *str) {
    if (!str || *str == '\0')
        return -1;

    while (*str == ' ' || *str == '\t')
        str++;

    if (*str < '0' || *str > '9')
        return -1;

    uint64_t value = 0;
    while (*str >= '0' && *str <= '9') {
        uint64_t next = value * 10 + (*str - '0');
        if (next < value)
            return -1;

        value = next;
        str++;
    }

    while (*str == ' ' || *str == '\t')
        str++;

    uint64_t multiplier = 1;
    switch (*str) {
    case 'K':
    case 'k':
        multiplier = 1024ULL;
        str++;
        break;
    case 'M':
    case 'm':
        multiplier = 1024ULL * 1024ULL;
        str++;
        break;
    case 'G':
    case 'g':
        multiplier = 1024ULL * 1024ULL * 1024ULL;
        str++;
        break;
    case 'T':
    case 't':
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        str++;
        break;
    case 'B':
    case 'b':
        multiplier = 1ULL;
        str++;
        break;
    case '\0': break;
    default: return -1;
    }

    if (*str == 'i' || *str == 'I')
        str++;
    if (*str == 'b' || *str == 'B')
        str++;

    while (*str == ' ' || *str == '\t')
        str++;

    if (*str != '\0')
        return -1;

    uint64_t result = value * multiplier;
    if (multiplier > 1 && result / multiplier != value)
        return -1;

    if (result > (uint64_t) INT64_MAX)
        return -1;

    return (ssize_t) result;
}

time_ns_t parse_duration(const char *str) {
    if (!str || *str == '\0')
        return TIME_NS_MAX;

    time_ns_t value = 0;
    while (*str >= '0' && *str <= '9') {
        value = value * 10 + (*str - '0');
        str++;
    }

    while (*str == ' ' || *str == '\t')
        str++;

    time_ns_t multiplier = 0;
    if (*str == 's' && *(str + 1) == '\0') {
        multiplier = 1000000000ULL;
    } else if (*str == 'm' && *(str + 1) == 's' && *(str + 2) == '\0') {
        multiplier = 1000000ULL;
    } else if (*str == 'u' && *(str + 1) == 's' && *(str + 2) == '\0') {
        multiplier = 1000ULL;
    } else if (*str == 'n' && *(str + 1) == 's' && *(str + 2) == '\0') {
        multiplier = 1ULL;
    } else if (*str == '\0') {
        multiplier = 1ULL;
    } else {
        return TIME_NS_MAX;
    }

    return (value * multiplier);
}

struct cpu_mask parse_cpu_mask(const char *str, size_t n_cpus) {
    if (!str)
        return CPU_MASK_ERR(ERR_INVAL);

    struct cpu_mask mask;
    enum errno e = ERR_INVAL;

    if (!cpu_mask_init(&mask, n_cpus))
        return CPU_MASK_ERR(ERR_NO_MEM);

    const char *p = str;
    while (*p) {
        if (*p < '0' || *p > '9')
            goto err;

        size_t start = 0;
        while (*p >= '0' && *p <= '9') {
            start = start * 10 + (*p - '0');
            p++;
        }

        size_t end = start;
        if (*p == '-') {
            p++;

            if (*p < '0' || *p > '9')
                goto err;

            end = 0;
            while (*p >= '0' && *p <= '9') {
                end = end * 10 + (*p - '0');
                p++;
            }
        }

        if (start > end)
            goto err;

        if (end >= n_cpus)
            goto err;

        for (size_t i = start; i <= end; i++)
            cpu_mask_set(&mask, i);

        if (*p == ',') {
            p++;
            if (*p == '\0')
                goto err;
        } else if (*p != '\0') {
            goto err;
        }
    }

    return mask;

err:
    cpu_mask_deinit(&mask);
    return CPU_MASK_ERR(e);
}

static int hex_to_int(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;

    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

uint64_t parse_mac(const char *str) {
    if (!str)
        return MAC_INVALID;

    uint64_t mac = 0;
    for (int i = 0; i < 6; i++) {
        int high = hex_to_int(*str++);
        if (high < 0)
            return MAC_INVALID;

        int low = hex_to_int(*str++);
        if (low < 0)
            return MAC_INVALID;

        mac = (mac << 8) | (high << 4) | low;

        if (i < 5) {
            if (*str != ':')
                return MAC_INVALID;
            str++;
        }
    }

    if (*str != '\0')
        return MAC_INVALID;

    return mac;
}
