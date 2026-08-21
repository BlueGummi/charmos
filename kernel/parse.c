#include <kassert.h>
#include <math/fixed.h>
#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <parse.h>
#include <string.h>

static bool parse_bool_internal(const char *str, bool *out) {
    if (!str || *str == '\0') {
        return false;
    }

    while (*str == ' ' || *str == '\t')
        str++;

    if (*str == '\0') {
        return false;
    }

    const char *end = str + strlen(str);
    while (end > str && (*(end - 1) == ' ' || *(end - 1) == '\t'))
        end--;

    size_t len = (size_t) (end - str);

    const char *enabled_terms[] = {
        "true",     "enabled", "y",      "yes",   "yeah", "yup", "on",
        "positive", "1",       "active", "allow", "ok",   "open"};

    const char *disabled_terms[] = {
        "false",    "disabled", "n",        "no",   "nope",    "off",
        "negative", "0",        "inactive", "deny", "blocked", "closed"};

    for (size_t i = 0; i < sizeof(enabled_terms) / sizeof(enabled_terms[0]);
         i++) {
        if (len == strlen(enabled_terms[i]) &&
            strncasecmp(str, enabled_terms[i], len) == 0) {
            if (out)
                *out = true;
            return true;
        }
    }

    for (size_t i = 0; i < sizeof(disabled_terms) / sizeof(disabled_terms[0]);
         i++) {
        if (len == strlen(disabled_terms[i]) &&
            strncasecmp(str, disabled_terms[i], len) == 0) {
            if (out)
                *out = false;
            return true;
        }
    }

    return false;
}

static bool parse_data_size_internal(const char *str, uint64_t *out) {
    if (!str || *str == '\0')
        return false;

    while (*str == ' ' || *str == '\t')
        str++;

    if (*str < '0' || *str > '9')
        return false;

    uint64_t value = 0;
    while (*str >= '0' && *str <= '9') {
        uint64_t digit = (uint64_t) (*str - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false; /* Overflow */

        value = value * 10 + digit;
        str++;
    }

    while (*str == ' ' || *str == '\t')
        str++;

    uint64_t multiplier = 1;
    bool has_prefix = false;
    switch (*str) {
    case 'K':
    case 'k':
        multiplier = 1024ULL;
        has_prefix = true;
        str++;
        break;
    case 'M':
    case 'm':
        multiplier = 1024ULL * 1024ULL;
        has_prefix = true;
        str++;
        break;
    case 'G':
    case 'g':
        multiplier = 1024ULL * 1024ULL * 1024ULL;
        has_prefix = true;
        str++;
        break;
    case 'T':
    case 't':
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        has_prefix = true;
        str++;
        break;
    case 'B':
    case 'b':
        multiplier = 1ULL;
        str++;
        break;
    case '\0': break;
    default: return false;
    }

    if (has_prefix) {
        if (*str == 'i' || *str == 'I') {
            str++;
            if (*str == 'b' || *str == 'B') {
                str++;
            } else {
                return false;
            }
        } else if (*str == 'b' || *str == 'B') {
            str++;
        }
    }

    while (*str == ' ' || *str == '\t')
        str++;

    if (*str != '\0')
        return false;

    uint64_t result = value * multiplier;
    if (multiplier > 1 && result / multiplier != value)
        return false; /* Overflow */

    if (out)
        *out = result;
    return true;
}

static bool parse_duration_internal(const char *str, time_ns_t *out) {
    if (!str || *str == '\0')
        return false;

    while (*str == ' ' || *str == '\t')
        str++;

    if (*str < '0' || *str > '9')
        return false;

    time_ns_t value = 0;
    while (*str >= '0' && *str <= '9') {
        time_ns_t digit = (time_ns_t) (*str - '0');
        if (value > (TIME_NS_MAX - digit) / 10)
            return false;

        value = value * 10 + digit;
        str++;
    }

    while (*str == ' ' || *str == '\t')
        str++;

    const char *unit_start = str;
    while (*str && *str != ' ' && *str != '\t')
        str++;

    size_t unit_len = str - unit_start;

    while (*str == ' ' || *str == '\t')
        str++;

    if (*str != '\0')
        return false;

    time_ns_t multiplier = 0;
    if (unit_len == 0) {
        multiplier = 1ULL;
    } else if (unit_len == 2 && strncasecmp(unit_start, "ns", 2) == 0) {
        multiplier = 1ULL;
    } else if (unit_len == 2 && strncasecmp(unit_start, "us", 2) == 0) {
        multiplier = 1000ULL;
    } else if (unit_len == 2 && strncasecmp(unit_start, "ms", 2) == 0) {
        multiplier = 1000000ULL;
    } else if ((unit_len == 1 && strncasecmp(unit_start, "s", 1) == 0) ||
               (unit_len == 3 && strncasecmp(unit_start, "sec", 3) == 0) ||
               (unit_len == 4 && strncasecmp(unit_start, "secs", 4) == 0) ||
               (unit_len == 6 && strncasecmp(unit_start, "second", 6) == 0) ||
               (unit_len == 7 && strncasecmp(unit_start, "seconds", 7) == 0)) {
        multiplier = 1000000000ULL;
    } else if ((unit_len == 1 && strncasecmp(unit_start, "m", 1) == 0) ||
               (unit_len == 3 && strncasecmp(unit_start, "min", 3) == 0) ||
               (unit_len == 4 && strncasecmp(unit_start, "mins", 4) == 0) ||
               (unit_len == 6 && strncasecmp(unit_start, "minute", 6) == 0) ||
               (unit_len == 7 && strncasecmp(unit_start, "minutes", 7) == 0)) {
        multiplier = 60ULL * 1000000000ULL;
    } else if ((unit_len == 1 && strncasecmp(unit_start, "h", 1) == 0) ||
               (unit_len == 2 && strncasecmp(unit_start, "hr", 2) == 0) ||
               (unit_len == 3 && strncasecmp(unit_start, "hrs", 3) == 0) ||
               (unit_len == 4 && strncasecmp(unit_start, "hour", 4) == 0) ||
               (unit_len == 5 && strncasecmp(unit_start, "hours", 5) == 0)) {
        multiplier = 3600ULL * 1000000000ULL;
    } else if ((unit_len == 1 && strncasecmp(unit_start, "d", 1) == 0) ||
               (unit_len == 3 && strncasecmp(unit_start, "day", 3) == 0) ||
               (unit_len == 4 && strncasecmp(unit_start, "days", 4) == 0)) {
        multiplier = 86400ULL * 1000000000ULL;
    } else {
        return false;
    }

    if (multiplier > 0 && value > TIME_NS_MAX / multiplier)
        return false;

    if (out)
        *out = (value * multiplier);
    return true;
}

static bool parse_cpu_mask_internal(const char *str, struct cpu_mask *out,
                                    size_t n_cpus) {
    if (!str || *str == '\0' || n_cpus == 0)
        return false;

    /* validate syntax and range */
    const char *p = str;
    while (*p) {
        if (*p < '0' || *p > '9')
            return false;

        size_t start = 0;
        while (*p >= '0' && *p <= '9') {
            size_t digit = (size_t) (*p - '0');
            if (start > (SIZE_MAX - digit) / 10)
                return false;

            start = start * 10 + digit;
            p++;
        }

        size_t end = start;
        if (*p == '-') {
            p++;
            if (*p < '0' || *p > '9')
                return false;

            end = 0;
            while (*p >= '0' && *p <= '9') {
                size_t digit = (size_t) (*p - '0');
                if (end > (SIZE_MAX - digit) / 10)
                    return false;

                end = end * 10 + digit;
                p++;
            }
        }

        if (start > end || end >= n_cpus)
            return false;

        if (*p == ',') {
            p++;
            if (*p == '\0')
                return false;

        } else if (*p != '\0') {
            return false;
        }
    }

    if (!out)
        return true;

    /* populate mask if requested */
    if (!cpu_mask_init(out, n_cpus))
        return false;

    p = str;
    while (*p) {
        size_t start = 0;
        while (*p >= '0' && *p <= '9') {
            start = start * 10 + (size_t) (*p - '0');
            p++;
        }

        size_t end = start;
        if (*p == '-') {
            p++;
            end = 0;
            while (*p >= '0' && *p <= '9') {
                end = end * 10 + (size_t) (*p - '0');
                p++;
            }
        }

        for (size_t i = start; i <= end; i++)
            cpu_mask_set(out, i);

        if (*p == ',')
            p++;
    }

    return true;
}

static bool parse_fx_internal(const char *str, fx32_32_t *out);

static bool parse_range_internal(const char *str, uint64_t *start,
                                 uint64_t *end) {
    if (!str || *str == '\0')
        return false;

    while (*str == ' ' || *str == '\t')
        str++;

    if (*str == '-' || *str == '\0')
        return false;

    const char *sep = strstr(str, "..");
    size_t sep_len = 2;
    if (!sep) {
        sep = strchr(str + 1, '-');
        sep_len = 1;
    }

    if (!sep)
        return false;

    size_t left_len = sep - str;
    const char *right_str = sep + sep_len;

    while (left_len > 0 &&
           (str[left_len - 1] == ' ' || str[left_len - 1] == '\t'))
        left_len--;

    while (*right_str == ' ' || *right_str == '\t')
        right_str++;

    if (left_len == 0 || *right_str == '\0')
        return false;

    char left_buf[64];
    if (left_len >= sizeof(left_buf))
        return false;
    memcpy(left_buf, str, left_len);
    left_buf[left_len] = '\0';

    uint64_t s = 0, f = 0;
    bool parsed = false;

    time_ns_t ds = 0, df = 0;
    if (parse_duration_internal(left_buf, &ds) &&
        parse_duration_internal(right_str, &df)) {
        s = (uint64_t) ds;
        f = (uint64_t) df;
        parsed = true;
    }

    if (!parsed) {
        uint64_t sz_s = 0, sz_f = 0;
        if (parse_data_size_internal(left_buf, &sz_s) &&
            parse_data_size_internal(right_str, &sz_f)) {
            s = sz_s;
            f = sz_f;
            parsed = true;
        }
    }

    if (!parsed) {
        fx32_32_t fx_s = 0, fx_f = 0;
        if (parse_fx_internal(left_buf, &fx_s) &&
            parse_fx_internal(right_str, &fx_f)) {
            s = (uint64_t) fx_s;
            f = (uint64_t) fx_f;
            parsed = true;
        }
    }

    if (!parsed) {
        char *end_l = NULL;
        char *end_r = NULL;
        s = strtoull(left_buf, &end_l, 0);
        f = strtoull(right_str, &end_r, 0);
        if (end_l != left_buf && *end_l == '\0' && end_r != right_str &&
            *end_r == '\0') {
            parsed = true;
        }
    }

    if (!parsed || s > f)
        return false;

    if (start)
        *start = s;
    if (end)
        *end = f;
    return true;
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

static bool parse_mac_internal(const char *str, uint64_t *out) {
    if (!str || *str == '\0')
        return false;

    uint64_t mac = 0;
    const char *p = str;
    for (int i = 0; i < 6; i++) {
        int high = hex_to_int(*p++);
        if (high < 0)
            return false;

        int low = hex_to_int(*p++);
        if (low < 0)
            return false;

        mac = (mac << 8) | (high << 4) | low;

        if (i < 5) {
            if (*p != ':')
                return false;

            p++;
        }
    }

    if (*p != '\0')
        return false;

    if (out)
        *out = mac;
    return true;
}

static bool parse_fx_internal(const char *str, fx32_32_t *out) {
    if (!str || *str == '\0')
        return false;

    char *end = NULL;
    fx32_32_t val = fx_parse(str, &end);
    if (end == str || *end != '\0')
        return false;

    if (out)
        *out = val;
    return true;
}

static bool parse_int_internal(const char *str, int64_t *out) {
    if (!str || *str == '\0')
        return false;

    char *end = NULL;
    long val = strtol(str, &end, 0);
    if (end == str || *end != '\0')
        return false;

    if (out)
        *out = (int64_t) val;
    return true;
}

static bool parse_uint_internal(const char *str, uint64_t *out) {
    if (!str || *str == '\0')
        return false;

    const char *p = str;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '-')
        return false;

    char *end = NULL;
    unsigned long long val = strtoull(str, &end, 0);
    if (end == str || *end != '\0')
        return false;

    if (out)
        *out = (uint64_t) val;
    return true;
}

static void parse_unescape(char *dst, const char *begin, const char *end) {
    while (begin < end) {
        if (*begin == '\\' && begin + 1 < end) {
            begin++;
            switch (*begin) {
            case 'n': *dst++ = '\n'; break;
            case 't': *dst++ = '\t'; break;
            case 'r': *dst++ = '\r'; break;
            default: *dst++ = *begin; break;
            }
            begin++;
        } else {
            *dst++ = *begin++;
        }
    }
    *dst = '\0';
}

static bool parse_list_internal(const char *str, struct parse_list *out) {
    if (!str || *str == '\0')
        return false;

    size_t count = 1;
    bool quoted = false;
    for (const char *p = str; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            continue;
        }
        if (*p == '"')
            quoted = !quoted;
        else if (*p == ',' && !quoted)
            count++;
    }

    if (quoted)
        return false;

    const char *item_start = str;
    quoted = false;
    for (const char *p = str;; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            continue;
        }
        if (*p == '"')
            quoted = !quoted;
        if ((*p == ',' && !quoted) || *p == '\0') {
            const char *begin = item_start;
            const char *end = p;
            while (begin < end && (*begin == ' ' || *begin == '\t'))
                begin++;
            while (end > begin && (end[-1] == ' ' || end[-1] == '\t'))
                end--;
            if (begin == end)
                return false;

            if (*begin == '"') {
                if (end - begin < 2 || end[-1] != '"')
                    return false;
            }
            if (*p == '\0')
                break;
            item_start = p + 1;
        }
    }

    if (!out)
        return true;

    char **items = kmalloc(count * sizeof(char *), ALLOC_FLAGS_ZERO);
    if (!items)
        return false;

    item_start = str;
    size_t item = 0;
    quoted = false;
    for (const char *p = str;; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            continue;
        }
        if (*p == '"')
            quoted = !quoted;
        if ((*p == ',' && !quoted) || *p == '\0') {
            const char *begin = item_start;
            const char *end = p;
            while (begin < end && (*begin == ' ' || *begin == '\t'))
                begin++;
            while (end > begin && (end[-1] == ' ' || end[-1] == '\t'))
                end--;
            if (*begin == '"') {
                begin++;
                end--;
            }
            char *text = kmalloc((size_t) (end - begin) + 1, ALLOC_FLAGS_ZERO);
            if (!text) {
                for (size_t k = 0; k < item; k++)
                    kfree(items[k]);
                kfree(items);
                return false;
            }
            parse_unescape(text, begin, end);
            items[item++] = text;
            if (*p == '\0')
                break;
            item_start = p + 1;
        }
    }

    out->count = count;
    out->items = items;
    return true;
}

bool parse_is_bool(const char *str, bool *out) {
    return parse_bool_internal(str, out);
}

bool parse_is_data_size(const char *str, uint64_t *out) {
    return parse_data_size_internal(str, out);
}

bool parse_is_duration(const char *str, time_ns_t *out) {
    return parse_duration_internal(str, out);
}

bool parse_is_cpu_mask(const char *str, struct cpu_mask *out, size_t n_cpus) {
    return parse_cpu_mask_internal(str, out, n_cpus);
}

bool parse_is_range(const char *str, uint64_t *start, uint64_t *end) {
    return parse_range_internal(str, start, end);
}

bool parse_is_mac(const char *str, uint64_t *out) {
    return parse_mac_internal(str, out);
}

bool parse_is_fx(const char *str, fx32_32_t *out) {
    return parse_fx_internal(str, out);
}

bool parse_is_int(const char *str, int64_t *out) {
    return parse_int_internal(str, out);
}

bool parse_is_uint(const char *str, uint64_t *out) {
    return parse_uint_internal(str, out);
}

bool parse_is_list(const char *str, struct parse_list *out) {
    return parse_list_internal(str, out);
}

void parse_list_free(struct parse_list *list) {
    if (!list || !list->items)
        return;
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i])
            kfree(list->items[i]);
    }
    kfree(list->items);
    list->items = NULL;
    list->count = 0;
}
