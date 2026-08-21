#include "internal.h"

static enum errno cmdline_parse_i64_val(const char *text, int64_t *out) {
    int64_t v;
    if (parse_is_int(text, &v)) {
        *out = v;
        return ERR_OK;
    }

    uint64_t size;
    if (parse_is_data_size(text, &size)) {
        if (size > (uint64_t) INT64_MAX)
            return ERR_OVERFLOW;
        *out = (int64_t) size;
        return ERR_OK;
    }

    return ERR_INVAL;
}

static enum errno cmdline_parse_u64_val(const char *text, uint64_t *out) {
    uint64_t v;
    if (parse_is_uint(text, &v)) {
        *out = v;
        return ERR_OK;
    }

    uint64_t size;
    if (parse_is_data_size(text, &size)) {
        *out = size;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_parse_i64(void *write_to, const char *text) {
    return cmdline_parse_i64_val(text, (int64_t *) write_to);
}

enum errno cmdline_parse_u64(void *write_to, const char *text) {
    return cmdline_parse_u64_val(text, (uint64_t *) write_to);
}

enum errno cmdline_parse_bool(void *write_to, const char *text) {
    bool out = false;
    if (!parse_is_bool(text, &out))
        return ERR_INVAL;

    *(bool *) write_to = out;
    return ERR_OK;
}

enum errno cmdline_parse_fx(void *write_to, const char *text) {
    fx32_32_t val = 0;
    if (!parse_is_fx(text, &val))
        return ERR_INVAL;
    *(fx32_32_t *) write_to = val;
    return ERR_OK;
}

enum errno cmdline_parse_duration(void *write_to, const char *text) {
    time_ns_t val = 0;
    if (!parse_is_duration(text, &val))
        return ERR_INVAL;
    *(time_ns_t *) write_to = val;
    return ERR_OK;
}

enum errno cmdline_parse_data_size(void *write_to, const char *text) {
    uint64_t val = 0;
    if (!parse_is_data_size(text, &val))
        return ERR_INVAL;
    *(uint64_t *) write_to = val;
    return ERR_OK;
}

enum errno cmdline_parse_cpu_mask(void *write_to, const char *text) {
    struct cpu_mask mask;
    if (!parse_is_cpu_mask(text, &mask, global.core_count))
        return ERR_INVAL;
    *(struct cpu_mask *) write_to = mask;
    return ERR_OK;
}

enum errno cmdline_parse_mac(void *write_to, const char *text) {
    uint64_t val = 0;
    if (!parse_is_mac(text, &val))
        return ERR_INVAL;
    *(uint64_t *) write_to = val;
    return ERR_OK;
}

enum errno cmdline_parse_range(void *write_to, const char *text) {
    uint64_t start = 0, end = 0;
    if (!parse_is_range(text, &start, &end))
        return ERR_INVAL;
    struct cmdline_range *range = (struct cmdline_range *) write_to;
    range->start = start;
    range->end = end;
    return ERR_OK;
}

enum errno cmdline_parse_string(void *write_to, const char *text) {
    if (!text)
        return ERR_INVAL;
    size_t len = strlen(text);
    char *copy = kmalloc_or_die(len + 1);
    memcpy(copy, text, len + 1);
    *(char **) write_to = copy;
    return ERR_OK;
}

static bool detect_bool(const char *text, void *out) {
    const char *p = text;
    while (*p == ' ' || *p == '\t')
        p++;

    if (strcmp(p, "0") == 0 || strcmp(p, "1") == 0)
        return false;

    return parse_is_bool(text, (bool *) out);
}

static bool detect_duration(const char *text, void *out) {
    const char *p = text;
    while (*p == ' ' || *p == '\t')
        p++;

    while (*p >= '0' && *p <= '9')
        p++;

    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '\0')
        return false;

    return parse_is_duration(text, (time_ns_t *) out);
}

static bool detect_data_size(const char *text, void *out) {
    const char *p = text;
    while (*p == ' ' || *p == '\t')
        p++;

    while (*p >= '0' && *p <= '9')
        p++;

    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '\0')
        return false;

    return parse_is_data_size(text, (uint64_t *) out);
}

static bool detect_cpu_mask(const char *text, void *out) {
    return parse_is_cpu_mask(text, (struct cpu_mask *) out, global.core_count);
}

static bool detect_range(const char *text, void *out) {
    if (!out)
        return parse_is_range(text, NULL, NULL);

    struct cmdline_range *r = (struct cmdline_range *) out;
    return parse_is_range(text, &r->start, &r->end);
}

static bool detect_mac(const char *text, void *out) {
    return parse_is_mac(text, (uint64_t *) out);
}

static bool detect_fx(const char *text, void *out) {
    if (strchr(text, '.') == NULL)
        return false;

    return parse_is_fx(text, (fx32_32_t *) out);
}

static bool detect_uint(const char *text, void *out) {
    return parse_is_uint(text, (uint64_t *) out);
}

static bool detect_int(const char *text, void *out) {
    return parse_is_int(text, (int64_t *) out);
}

static bool detect_string(const char *text, void *out) {
    (void) out;
    return text != NULL;
}

const struct cmdline_type_parser cmdline_parsers[] = {
    {
        .type = CMDLINE_TYPE_BOOL,
        .name = "bool",
        .arg_hint = "<on/off>",
        .value_size = sizeof(bool),
        .uses_allocated_ptr = false,
        .detect = detect_bool,
        .parse = cmdline_parse_bool,
    },
    {
        .type = CMDLINE_TYPE_DURATION,
        .name = "duration",
        .arg_hint = "<time>",
        .value_size = sizeof(time_ns_t),
        .uses_allocated_ptr = false,
        .detect = detect_duration,
        .parse = cmdline_parse_duration,
    },
    {
        .type = CMDLINE_TYPE_DATA_SIZE,
        .name = "data_size",
        .arg_hint = "<size>",
        .value_size = sizeof(uint64_t),
        .uses_allocated_ptr = false,
        .detect = detect_data_size,
        .parse = cmdline_parse_data_size,
    },
    {
        .type = CMDLINE_TYPE_CPU_MASK,
        .name = "cpu_mask",
        .arg_hint = "<cpus>",
        .value_size = sizeof(struct cpu_mask),
        .uses_allocated_ptr = true,
        .detect = detect_cpu_mask,
        .parse = cmdline_parse_cpu_mask,
    },
    {
        .type = CMDLINE_TYPE_RANGE,
        .name = "range",
        .arg_hint = "<start-end>",
        .value_size = sizeof(struct cmdline_range),
        .uses_allocated_ptr = true,
        .detect = detect_range,
        .parse = cmdline_parse_range,
    },
    {
        .type = CMDLINE_TYPE_MAC,
        .name = "mac",
        .arg_hint = "<mac-addr>",
        .value_size = sizeof(uint64_t),
        .uses_allocated_ptr = false,
        .detect = detect_mac,
        .parse = cmdline_parse_mac,
    },
    {
        .type = CMDLINE_TYPE_FX,
        .name = "fixed_point",
        .arg_hint = "<float>",
        .value_size = sizeof(fx32_32_t),
        .uses_allocated_ptr = false,
        .detect = detect_fx,
        .parse = cmdline_parse_fx,
    },
    {
        .type = CMDLINE_TYPE_UINT,
        .name = "uint",
        .arg_hint = "<unsigned-int>",
        .value_size = sizeof(uint64_t),
        .uses_allocated_ptr = false,
        .detect = detect_uint,
        .parse = cmdline_parse_u64,
    },
    {
        .type = CMDLINE_TYPE_INT,
        .name = "int",
        .arg_hint = "<integer>",
        .value_size = sizeof(int64_t),
        .uses_allocated_ptr = false,
        .detect = detect_int,
        .parse = cmdline_parse_i64,
    },
    {
        .type = CMDLINE_TYPE_STRING,
        .name = "string",
        .arg_hint = "<string>",
        .value_size = sizeof(char *),
        .uses_allocated_ptr = true,
        .detect = detect_string,
        .parse = cmdline_parse_string,
    },
};

const size_t cmdline_parsers_count =
    sizeof(cmdline_parsers) / sizeof(cmdline_parsers[0]);

#define CMDLINE_AUTO_ARG_POOL_MAX 16
#define CMDLINE_AUTO_ARG_LEN 64

struct cmdline_auto_arg_entry {
    uint64_t mask;
    char str[CMDLINE_AUTO_ARG_LEN];
};

static struct cmdline_auto_arg_entry auto_arg_pool[CMDLINE_AUTO_ARG_POOL_MAX];
static size_t auto_arg_pool_count = 0;

const char *cmdline_intern_composite_arg(uint64_t mask,
                                         const enum cmdline_type *types,
                                         size_t num_types) {
    for (size_t i = 0; i < auto_arg_pool_count; i++) {
        if (auto_arg_pool[i].mask == mask)
            return auto_arg_pool[i].str;
    }

    kassert(auto_arg_pool_count < CMDLINE_AUTO_ARG_POOL_MAX);
    struct cmdline_auto_arg_entry *slot = &auto_arg_pool[auto_arg_pool_count++];
    slot->mask = mask;

    char temp[CMDLINE_AUTO_ARG_LEN];
    size_t idx = 0;
    temp[idx++] = '<';

    for (size_t i = 0; i < num_types; i++) {
        const char *hint = cmdline_type_raw_hint(types[i]);
        if (!hint)
            continue;
        size_t len = strlen(hint);
        if (idx + len + 2 < CMDLINE_AUTO_ARG_LEN) {
            memcpy(&temp[idx], hint, len);
            idx += len;
            if (i + 1 < num_types)
                temp[idx++] = '|';
        }
    }

    temp[idx++] = '>';
    temp[idx] = '\0';

    memcpy(slot->str, temp, idx + 1);
    return slot->str;
}

void cmdline_assign_all_args(void) {
    static bool assigned = false;
    if (assigned)
        return;
    assigned = true;

    for (struct cmdline_entry *ent = __skernel_cmdline_entries;
         ent < __ekernel_cmdline_entries; ent++) {
        if (ent->flags & (CMDLINE_ENTRY_HIDDEN | CMDLINE_ENTRY_SYMBOLIC))
            continue;
        if (ent->arg != NULL)
            continue;

        if (ent->value.mode == CMDLINE_MODE_TYPED) {
            enum cmdline_type cmd_t =
                cmdline_type_enum_to_cmdline_type(ent->value.c_type);
            if (cmd_t != CMDLINE_TYPE_NONE)
                ent->arg = CMDLINE_EXPR_TYPE_TO_STR(cmd_t);
        } else if (ent->value.mode == CMDLINE_MODE_CUSTOM) {
            for (size_t i = 0; i < cmdline_parsers_count; i++) {
                if (cmdline_parsers[i].parse == ent->value.parse) {
                    ent->arg = cmdline_parsers[i].arg_hint;
                    break;
                }
            }
        } else if (ent->value.mode == CMDLINE_MODE_POLYMORPHIC) {
            uint64_t mask = cmdline_entry_get_accepted_mask(ent);
            if (mask == UINT64_MAX || mask == 0)
                continue;

            size_t count = popcount(mask);
            if (count == 0)
                continue;

            enum cmdline_type types[CMDLINE_MAX_TYPE_ARGS];
            size_t num_types = cmdline_extract_type_bits(mask, types);

            if (num_types == 1) {
                ent->arg = CMDLINE_EXPR_TYPE_TO_STR(types[0]);
            } else if (num_types > 1) {
                ent->arg = cmdline_intern_composite_arg(mask, types, num_types);
            }
        }
    }
}
