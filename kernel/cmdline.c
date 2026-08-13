#include <asm.h>
#include <cmdline.h>
#include <console/panic.h>
#include <console/printf.h>
#include <errno.h>
#include <fs/vfs.h>
#include <global.h>
#include <kassert.h>
#include <math/fixed.h>
#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <parse.h>
#include <string.h>

#define MAX_VAR_LEN 128
#define MAX_VAL_LEN 256

static struct cmdline_value cmdline_parse_value_for(const char *value,
                                                    uint64_t accepted);

static inline bool cmdline_type_is_cmdline_value(int type) {
    kassert(type <= CMDLINE_VAL_MAX);
    return type >= CMDLINE_VAL_NONE;
}

static inline bool cmdline_type_is_type_enum(int type) {
    return !cmdline_type_is_cmdline_value(type);
}

static enum errno cmdline_parse_long(const char *text, long *out) {
    char *end;
    long v = strtol(text, &end, 0);
    if (end != text && *end == '\0') {
        *out = v;
        return ERR_OK;
    }

    ssize_t size = parse_data_size(text);
    if (size >= 0) {
        *out = (long) size;
        return ERR_OK;
    }

    return ERR_INVAL;
}

static enum errno cmdline_parse_ulong(const char *text, uint64_t *out) {
    /* strtoull would silently wrap a leading '-'; reject it for unsigned. */
    const char *p = text;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '-')
        return ERR_INVAL;

    char *end;
    unsigned long long v = strtoull(text, &end, 0);
    if (end != text && *end == '\0') {
        *out = (uint64_t) v;
        return ERR_OK;
    }

    ssize_t size = parse_data_size(text);
    if (size >= 0) {
        *out = (uint64_t) size;
        return ERR_OK;
    }

    return ERR_INVAL;
}

#define CMDLINE_DEFINE_UINT_PARSER(fn, ctype, cmax)                            \
    enum errno fn(void *write_to, const char *text) {                          \
        uint64_t v;                                                            \
        enum errno err = cmdline_parse_ulong(text, &v);                        \
        if (err != ERR_OK)                                                     \
            return err;                                                        \
        if (v > (cmax))                                                        \
            return ERR_OVERFLOW;                                               \
        *(ctype *) write_to = (ctype) v;                                       \
        return ERR_OK;                                                         \
    }

#define CMDLINE_DEFINE_INT_PARSER(fn, ctype, cmin, cmax)                       \
    enum errno fn(void *write_to, const char *text) {                          \
        long v;                                                                \
        enum errno err = cmdline_parse_long(text, &v);                         \
        if (err != ERR_OK)                                                     \
            return err;                                                        \
        if (v < (cmin) || v > (cmax))                                          \
            return ERR_OVERFLOW;                                               \
        *(ctype *) write_to = (ctype) v;                                       \
        return ERR_OK;                                                         \
    }

CMDLINE_DEFINE_UINT_PARSER(cmdline_parse_u8, uint8_t, UINT8_MAX)
CMDLINE_DEFINE_UINT_PARSER(cmdline_parse_u16, uint16_t, UINT16_MAX)
CMDLINE_DEFINE_UINT_PARSER(cmdline_parse_u32, uint32_t, UINT32_MAX)
CMDLINE_DEFINE_INT_PARSER(cmdline_parse_i8, int8_t, INT8_MIN, INT8_MAX)
CMDLINE_DEFINE_INT_PARSER(cmdline_parse_i16, int16_t, INT16_MIN, INT16_MAX)
CMDLINE_DEFINE_INT_PARSER(cmdline_parse_i32, int32_t, INT32_MIN, INT32_MAX)

enum errno cmdline_parse_i64(void *write_to, const char *text) {
    return cmdline_parse_long(text, (long *) write_to);
}

/* u64 uses strtoull, so a full 64-bit value (e.g. a hex seed above INT64_MAX)
 * round-trips correctly. */
enum errno cmdline_parse_u64(void *write_to, const char *text) {
    return cmdline_parse_ulong(text, (uint64_t *) write_to);
}

enum errno cmdline_parse_bool(void *write_to, const char *text) {
    *(bool *) write_to = parse_bool(text);

    return ERR_OK;
}

enum errno cmdline_parse_float(void *write_to, const char *text) {
    (void) write_to;
    panic("cmdline: floating-point values are unsupported (kernel builds "
          "without FP), got '%s'",
          text);
}

enum errno cmdline_parse_unsupported(void *write_to, const char *text) {
    (void) write_to;
    panic("cmdline: no parser for the requested type (value '%s')", text);
}

enum errno cmdline_parse_fx(void *write_to, const char *text) {
    char *end;
    fx32_32_t v = fx_parse(text, &end);
    if (end == text || *end != '\0')
        return ERR_INVAL;
    *(fx32_32_t *) write_to = v;
    return ERR_OK;
}

enum errno (*cmdline_parse_table[TYPE_MAX])(void *write_to,
                                            const char *text) = {
    [TYPE_NONE] = cmdline_parse_unsupported,
    [TYPE_INT8] = cmdline_parse_i8,
    [TYPE_UINT8] = cmdline_parse_u8,
    [TYPE_INT16] = cmdline_parse_i16,
    [TYPE_UINT16] = cmdline_parse_u16,
    [TYPE_INT32] = cmdline_parse_i32,
    [TYPE_UINT32] = cmdline_parse_u32,
    [TYPE_INT64] = cmdline_parse_i64,
    [TYPE_UINT64] = cmdline_parse_u64,
    [TYPE_FLOAT32] = cmdline_parse_float,
    [TYPE_FLOAT64] = cmdline_parse_float,
    [TYPE_BOOL] = cmdline_parse_bool,
    [TYPE_POINTER] = cmdline_parse_unsupported,
    [TYPE_UNKNOWN] = cmdline_parse_unsupported,
};

CMDLINE_ENTRY_DECLARE(root,
                      .desc = "Root filesystem partition to mount at boot",
                      .arg = "<device>", .default_val = NULL,
                      .raw = &global.root_partition,
                      .flags = CMDLINE_ENTRY_REQUIRED);

static void get_functional_name(struct cmdline_entry *ent,
                                char name_out[CMDLINE_ENTRY_NAME_LEN_MAX]) {
    /* Find the root, then the next one down,
     * then the next, until we get to this */
    struct cmdline_entry *stop_at = NULL;
    size_t idx = 0;

    while (true) {
        struct cmdline_entry *curr = ent;
        struct cmdline_entry *prev = curr;
        while ((curr = prev->parent) != stop_at)
            prev = curr;

        const char *name = prev->name;
        size_t len = strlen(name);
        memcpy(name_out + idx, name, len);
        idx += len;

        if (prev == ent) {
            /* prev == ent, we are at the root, break */
            name_out[idx] = '\0';
            return;
        } else {
            /* append a period */
            name_out[idx++] = '.';
            stop_at = prev;
        }
    }
}

static void cmdline_check_for_duplicates(void) {
    size_t dupes = 0;
    for (struct cmdline_entry *a = __skernel_cmdline_entries;
         a < __ekernel_cmdline_entries; a++) {
        char name_a[CMDLINE_ENTRY_NAME_LEN_MAX];
        get_functional_name(a, name_a);

        for (struct cmdline_entry *b = a + 1; b < __ekernel_cmdline_entries;
             b++) {
            char name_b[CMDLINE_ENTRY_NAME_LEN_MAX];
            get_functional_name(b, name_b);
            if (strcmp(name_a, name_b) == 0) {
                log_msg(LOG_ERROR, "duplicate command line entry: %s", name_a);
                dupes++;
            }
        }
    }
    if (dupes)
        panic("%zu duplicate command line entries", dupes);
}

static void cmdline_check_for_unfilled(void) {
    size_t found = 0;
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        if ((e->flags & CMDLINE_ENTRY_REQUIRED) &&
            e->status == CMDLINE_ENTRY_NOT_FOUND) {
            char name[CMDLINE_ENTRY_NAME_LEN_MAX];
            get_functional_name(e, name);
            log_msg(LOG_ERROR, "Required command line entry %s not present",
                    name);
            found++;
        }
    }
    if (found)
        panic("%zu required command line entries not present", found);
}

static void cmdline_apply_defaults(void) {
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        if (e->status == CMDLINE_ENTRY_NOT_FOUND && e->default_val) {
            if (e->raw)
                *e->raw = (char *) e->default_val;
            else if (e->callback)
                e->callback(e->default_val, e);
            e->status = CMDLINE_ENTRY_DEFAULTED;
            char name[CMDLINE_ENTRY_NAME_LEN_MAX];
            get_functional_name(e, name);
            log_msg(LOG_INFO, "command line entry '%s' defaulted to '%s'", name,
                    e->default_val);
        }
    }
}

static void cmdline_dispatch(const char *var, const char *val) {
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        kassert(e->name);

        if (e->flags & CMDLINE_ENTRY_SYMBOLIC)
            continue;

        char name[CMDLINE_ENTRY_NAME_LEN_MAX];
        get_functional_name(e, name);

        if (strcmp(name, var) != 0)
            continue;

        if (e->status == CMDLINE_ENTRY_FOUND)
            panic("duplicate cmdline entry: %s", var);

        e->status = CMDLINE_ENTRY_FOUND;
        if (cmdline_type_is_type_enum(e->value.type) && e->value.parse) {
            e->value.parse(e->value.write_to, val);
        } else {
            struct cmdline_value vtmp =
                cmdline_parse_value_for(val, e->value.accepted);
            kassert(e->value.accepted & CMDLINE_VALUE_TYPE_BIT(vtmp.type),
                    "cmdline variable %s received incompatible type %s", var,
                    cmdline_value_type_to_str(vtmp.type));
            e->value = vtmp;
        }

        if (e->callback) {
            e->callback(val, e);
        } else if (e->raw) {
            char *copy = kmalloc_or_die(strlen(val) + 1);
            memcpy(copy, val, strlen(val) + 1);
            *e->raw = copy;
            log_msg(LOG_INFO, "command line entry '%s' set to '%s'", name,
                    copy);
        }

        return;
    }

    panic("unknown command line key '%s'", var);
}

static void cmdline_print_all() {
#ifdef DEBUG_CMDLINE
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        if (e->flags & CMDLINE_ENTRY_SYMBOLIC)
            continue;

        char name[CMDLINE_ENTRY_NAME_LEN_MAX];
        get_functional_name(e, name);
        log_msg(LOG_INFO, "command line entry %s = %s", name,
                e->value ? *e->value : "(null)");
    }
#endif
}

static void cmdline_assign_all_parsers() {
    for (struct cmdline_entry *ent = __skernel_cmdline_entries;
         ent < __ekernel_cmdline_entries; ent++) {
        if (ent->value.type != TYPE_NONE &&
            ent->value.type != CMDLINE_VAL_NONE) {
            ent->value.parse = cmdline_parse_table[ent->value.type];
        }
    }
}

bool cmdline_wants_help(const char *input) {
    if (!input)
        return false;

    while (*input) {
        while (*input == ' ' || *input == '\t')
            input++;

        if (*input == '\0')
            break;

        const char *tok = input;
        while (*input && *input != ' ' && *input != '\t' && *input != '=')
            input++;

        size_t len = (size_t) (input - tok);
        if (len == 4 && memcmp(tok, "help", 4) == 0 && *input != '=')
            return true;

        /* skip the rest of this token */
        bool in_quote = false;
        while (*input && (in_quote || (*input != ' ' && *input != '\t'))) {
            if (*input == '\\' && *(input + 1) != '\0') {
                input += 2;
                continue;
            }
            if (*input == '"')
                in_quote = !in_quote;
            input++;
        }
    }
    return false;
}

__noreturn void cmdline_dump_help(void) {
    printf("charmos kernel command-line options:\n\n");
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        if (!(e->flags & CMDLINE_ENTRY_DOCUMENTED))
            continue;

        char name[CMDLINE_ENTRY_NAME_LEN_MAX];
        get_functional_name(e, name);
        printf("  %s%s%s\n", name, e->arg ? "=" : "", e->arg ? e->arg : "");
        if (e->desc)
            printf("      %s\n", e->desc);
        printf("      %s",
               (e->flags & CMDLINE_ENTRY_REQUIRED) ? "required" : "optional");
        if (e->default_val)
            printf(", default: %s", e->default_val);
        printf("\n\n");
    }
    printf("(kernel halted after `help`)\n");

    disable_interrupts();
    for (;;)
        hcf();
}

void cmdline_parse(const char *input) {
    char var_buf[MAX_VAR_LEN];
    char val_buf[MAX_VAL_LEN];

    cmdline_check_for_duplicates();
    cmdline_assign_all_parsers();

    while (*input) {
        while (*input == ' ' || *input == '\t')
            input++;

        if (*input == '\0')
            break;

        const char *var_start = input;
        while (*input && *input != '=' && *input != ' ' && *input != '\t')
            input++;

        const char *var_end = input;

        while (var_end > var_start &&
               (*(var_end - 1) == ' ' || *(var_end - 1) == '\t'))
            var_end--;

        while (*input && *input != '=' && *input != ' ' && *input != '\t')
            input++;

        if (*input != '=') {
            /* Skip tok without '=' */
            while (*input && *input != ' ' && *input != '\t')
                input++;
            continue;
        }

        input++; /* Skip '=' */

        while (*input == ' ' || *input == '\t')
            input++;

        size_t val_idx = 0;

        /* double quoted values (e.g. key="value with spaces and \"quotes\"") */
        if (*input == '"') {
            input++; /* Skip opening quote */
            while (*input) {
                if (*input == '\\' && *(input + 1) != '\0') {
                    input++; /* Skip backslash */
                    char escaped = *input;
                    switch (escaped) {
                    case 'n': escaped = '\n'; break;
                    case 't': escaped = '\t'; break;
                    case 'r': escaped = '\r'; break;
                    default: break;
                    }
                    if (val_idx < MAX_VAL_LEN - 1)
                        val_buf[val_idx++] = escaped;
                    input++;
                } else if (*input == '"') {
                    input++; /* Skip closing quote */
                    break;
                } else {
                    if (val_idx < MAX_VAL_LEN - 1)
                        val_buf[val_idx++] = *input;
                    input++;
                }
            }
        } else {
            while (*input && *input != ' ' && *input != '\t') {
                if (*input == '\\' && *(input + 1) != '\0') {
                    input++; /* Skip backslash */
                    char escaped = *input;
                    switch (escaped) {
                    case 'n': escaped = '\n'; break;
                    case 't': escaped = '\t'; break;
                    case 'r': escaped = '\r'; break;
                    default: break;
                    }
                    if (val_idx < MAX_VAL_LEN - 1)
                        val_buf[val_idx++] = escaped;
                    input++;
                } else {
                    if (val_idx < MAX_VAL_LEN - 1)
                        val_buf[val_idx++] = *input;
                    input++;
                }
            }
        }
        val_buf[val_idx] = '\0';

        size_t var_len = (size_t) (var_end - var_start);
        if (var_len >= MAX_VAR_LEN)
            var_len = MAX_VAR_LEN - 1;

        memcpy(var_buf, var_start, var_len);
        var_buf[var_len] = '\0';

        cmdline_dispatch(var_buf, val_buf);
    }

    cmdline_apply_defaults();
    cmdline_check_for_unfilled();
    cmdline_print_all();
}

static bool cmdline_detect_bool(const char *value, bool *out_bool) {
    if (!value)
        return false;

    const char *bool_true[] = {"true",   "enabled", "y",  "yes",
                               "yeah",   "yup",     "on", "positive",
                               "active", "allow",   "ok", "open"};
    const char *bool_false[] = {"false", "disabled", "n",        "no",
                                "nope",  "off",      "negative", "inactive",
                                "deny",  "blocked",  "closed"};

    for (size_t i = 0; i < sizeof(bool_true) / sizeof(bool_true[0]); i++) {
        if (strcasecmp(value, bool_true[i]) == 0) {
            if (out_bool)
                *out_bool = true;
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(bool_false) / sizeof(bool_false[0]); i++) {
        if (strcasecmp(value, bool_false[i]) == 0) {
            if (out_bool)
                *out_bool = false;
            return true;
        }
    }

    return false;
}

static bool cmdline_detect_data_size(const char *value, uint64_t *out_size) {
    if (!value)
        return false;

    bool has_size_suffix = false;
    const char *p = value;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '+' || *p == '-')
        p++;
    while (*p >= '0' && *p <= '9')
        p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '\0')
        has_size_suffix = true;

    if (!has_size_suffix)
        return false;

    ssize_t size = parse_data_size(value);
    if (size < 0)
        return false;

    if (out_size)
        *out_size = (uint64_t) size;
    return true;
}

static bool cmdline_detect_int(const char *value, uint64_t *out_int) {
    if (!value || *value == '\0')
        return false;

    char *end = NULL;
    uint64_t uval = strtoull(value, &end, 0);
    if (end == value || *end != '\0')
        return false;

    if (out_int)
        *out_int = uval;
    return true;
}

static bool cmdline_parse_range(const char *value, struct cmdline_range *out) {
    char *end;
    uint64_t start = strtoull(value, &end, 0);
    if (end == value || *end != '-')
        return false;

    const char *end_text = end + 1;
    if (*end_text == '\0')
        return false;
    uint64_t finish = strtoull(end_text, &end, 0);
    if (end == end_text || *end != '\0' || start > finish)
        return false;

    out->start = start;
    out->end = finish;
    return true;
}

static bool cmdline_has_list_separator(const char *value) {
    bool quoted = false;
    for (const char *p = value; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            continue;
        }
        if (*p == '"')
            quoted = !quoted;
        else if (*p == ',' && !quoted)
            return true;
    }
    return false;
}

static void cmdline_copy_escaped(char *out, const char *begin,
                                 const char *end) {
    while (begin < end) {
        if (*begin == '\\' && begin + 1 < end) {
            begin++;
            switch (*begin) {
            case 'n': *out++ = '\n'; break;
            case 't': *out++ = '\t'; break;
            case 'r': *out++ = '\r'; break;
            default: *out++ = *begin; break;
            }
            begin++;
        } else {
            *out++ = *begin++;
        }
    }
    *out = '\0';
}

static struct cmdline_value cmdline_parse_list(const char *value,
                                               uint64_t accepted) {
    size_t count = 1;
    bool quoted = false;
    for (const char *p = value; *p; p++) {
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
        panic("cmdline: unterminated quote in list '%s'", value);

    struct cmdline_list *list = kmalloc_or_die(sizeof(*list));
    list->count = count;
    list->items = kmalloc_or_die(count * sizeof(*list->items));

    const char *item_start = value;
    size_t item = 0;
    quoted = false;
    for (const char *p = value;; p++) {
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
                panic("cmdline: empty item in list '%s'", value);
            if (*begin == '"') {
                if (end - begin < 2 || end[-1] != '"')
                    panic("cmdline: malformed quoted list item in '%s'", value);
                begin++;
                end--;
            }
            char *text = kmalloc_or_die((size_t) (end - begin) + 1);
            cmdline_copy_escaped(text, begin, end);
            list->items[item] = cmdline_parse_value_for(text, accepted);
            item++;
            kfree(text);
            if (*p == '\0')
                break;
            item_start = p + 1;
        }
    }

    return (struct cmdline_value){.type = CMDLINE_VAL_LIST, .data = list};
}

static enum cmdline_value_type cmdline_detect_value_type(const char *value,
                                                         void **out_data) {
    enum cmdline_value_type type = CMDLINE_VAL_NONE;
    void *allocated_data = NULL;
    size_t alloc_size = 0;
    const void *src_ptr = NULL;

    if (!value)
        goto out;

    bool b_val;
    if (cmdline_detect_bool(value, &b_val)) {
        type = CMDLINE_VAL_BOOL;
        alloc_size = sizeof(bool);
        src_ptr = &b_val;
        goto out;
    }

    uint64_t size_val;
    if (cmdline_detect_data_size(value, &size_val)) {
        type = CMDLINE_VAL_DATA_SIZE;
        alloc_size = sizeof(uint64_t);
        src_ptr = &size_val;
        goto out;
    }

    uint64_t int_val;
    if (cmdline_detect_int(value, &int_val)) {
        type = CMDLINE_VAL_INT;
        alloc_size = sizeof(uint64_t);
        src_ptr = &int_val;
        goto out;
    }

    /* Fallback to string */
    type = CMDLINE_VAL_STRING;
    alloc_size = strlen(value) + 1;
    src_ptr = value;

out:
    if (out_data) {
        if (alloc_size > 0 && src_ptr) {
            allocated_data = kmalloc_or_die(alloc_size);
            memcpy(allocated_data, src_ptr, alloc_size);
        }
        *out_data = allocated_data;
    }

    return type;
}

static struct cmdline_value cmdline_parse_value_for(const char *value,
                                                    uint64_t accepted) {
    struct cmdline_value val = {
        .entry = NULL,
        .type = CMDLINE_VAL_NONE,
        .data = NULL,
    };

    if (!value)
        return val;

    bool wants_range = accepted & CMDLINE_VALUE_TYPE_BIT(CMDLINE_VAL_RANGE);
    bool wants_cpu_mask =
        accepted & CMDLINE_VALUE_TYPE_BIT(CMDLINE_VAL_CPU_MASK);
    if (wants_range && wants_cpu_mask && accepted != UINT64_MAX)
        panic("cmdline: RANGE and CPU_MASK cannot both be accepted");

    if ((accepted & CMDLINE_VALUE_TYPE_BIT(CMDLINE_VAL_LIST)) &&
        cmdline_has_list_separator(value))
        return cmdline_parse_list(value, accepted);

    if (wants_range) {
        struct cmdline_range *range = kmalloc_or_die(sizeof(*range));
        if (!cmdline_parse_range(value, range)) {
            kfree(range);
            return (struct cmdline_value){.type = CMDLINE_VAL_ERR};
        }
        return (struct cmdline_value){.type = CMDLINE_VAL_RANGE, .data = range};
    }

    if (wants_cpu_mask) {
        struct cpu_mask mask = parse_cpu_mask(value, global.core_count);
        if (mask.nbits == 0)
            panic("cmdline: invalid CPU mask '%s' (CPUs must be below %zu)",
                  value, global.core_count);
        struct cpu_mask *data = kmalloc_or_die(sizeof(*data));
        *data = mask;
        return (struct cmdline_value){.type = CMDLINE_VAL_CPU_MASK,
                                      .data = data};
    }

    val.type = cmdline_detect_value_type(value, &val.data);
    return val;
}

struct cmdline_value cmdline_parse_value(struct cmdline_entry *ent,
                                         const char *value) {
    return cmdline_parse_value_for(value, ent->value.accepted);
}

enum errno cmdline_extract_bool(struct cmdline_value *val, bool *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_VAL_BOOL) {
        if (val->data)
            *out = *(bool *) val->data;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_u64(struct cmdline_value *val, uint64_t *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_VAL_INT || val->type == CMDLINE_VAL_DATA_SIZE) {
        if (val->data)
            *out = *(uint64_t *) val->data;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_i64(struct cmdline_value *val, int64_t *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_VAL_INT || val->type == CMDLINE_VAL_DATA_SIZE) {
        if (val->data)
            *out = (int64_t) *(uint64_t *) val->data;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_u32(struct cmdline_value *val, uint32_t *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_VAL_INT || val->type == CMDLINE_VAL_DATA_SIZE) {
        if (val->data) {
            uint64_t v = *(uint64_t *) val->data;
            if (v > UINT32_MAX)
                return ERR_OVERFLOW;
            *out = (uint32_t) v;
        }
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_i32(struct cmdline_value *val, int32_t *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_VAL_INT || val->type == CMDLINE_VAL_DATA_SIZE) {
        if (val->data) {
            uint64_t v = *(uint64_t *) val->data;
            if (v > INT32_MAX)
                return ERR_OVERFLOW;
            *out = (int32_t) v;
        }
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_range(struct cmdline_value *val,
                                 struct cmdline_range *out) {
    if (!val || !out || val->type != CMDLINE_VAL_RANGE || !val->data)
        return ERR_INVAL;
    *out = *(struct cmdline_range *) val->data;
    return ERR_OK;
}

enum errno cmdline_extract_cpu_mask(struct cmdline_value *val,
                                    struct cpu_mask *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_VAL_CPU_MASK) {
        if (val->data)
            *out = *(struct cpu_mask *) val->data;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_string(struct cmdline_value *val, char **out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_VAL_STRING) {
        *out = (char *) val->data;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_const_string(struct cmdline_value *val,
                                        const char **out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_VAL_STRING) {
        *out = (const char *) val->data;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_list(struct cmdline_value *val,
                                struct cmdline_list *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_VAL_LIST) {
        if (val->data)
            *out = *(struct cmdline_list *) val->data;
        return ERR_OK;
    }

    return ERR_INVAL;
}
