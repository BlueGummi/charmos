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
#include <string.h>

#define MAX_VAR_LEN 128
#define MAX_VAL_LEN 256

static enum errno cmdline_parse_long(const char *text, long *out) {
    char *end;
    long v = strtol(text, &end, 0);
    if (end == text || *end != '\0')
        return ERR_INVAL;
    *out = v;
    return ERR_OK;
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
    if (end == text || *end != '\0')
        return ERR_INVAL;
    *out = (uint64_t) v;
    return ERR_OK;
}

#define CMDLINE_DEFINE_UINT_PARSER(fn, ctype, cmax)                            \
    static enum errno fn(void *write_to, const char *text) {                   \
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
    static enum errno fn(void *write_to, const char *text) {                   \
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

static enum errno cmdline_parse_i64(void *write_to, const char *text) {
    return cmdline_parse_long(text, (long *) write_to);
}

/* u64 uses strtoull, so a full 64-bit value (e.g. a hex seed above INT64_MAX)
 * round-trips correctly. */
static enum errno cmdline_parse_u64(void *write_to, const char *text) {
    return cmdline_parse_ulong(text, (uint64_t *) write_to);
}

static enum errno cmdline_parse_bool(void *write_to, const char *text) {
    *(bool *) write_to = cmdline_is_enabled(text);

    return ERR_OK;
}

static enum errno cmdline_parse_float(void *write_to, const char *text) {
    (void) write_to;
    panic("cmdline: floating-point values are unsupported (kernel builds "
          "without FP), got '%s'",
          text);
}

static enum errno cmdline_parse_unsupported(void *write_to, const char *text) {
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
                      .value = &global.root_partition,
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
            if (e->value)
                *e->value = (char *) e->default_val;
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
        if (e->parse) {
            e->parse(e->write_to, val);
        }

        if (e->callback) {
            e->callback(val, e);
        } else if (e->value) {
            char *copy = kmalloc_or_die(strlen(val) + 1);
            memcpy(copy, val, strlen(val) + 1);
            *e->value = copy;
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

bool cmdline_is_enabled(const char *str) {
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

static void cmdline_assign_all_parsers() {
    for (struct cmdline_entry *ent = __skernel_cmdline_entries;
         ent < __ekernel_cmdline_entries; ent++) {
        if (ent->type != TYPE_NONE) {
            ent->parse = cmdline_parse_table[ent->type];
        }
    }
}

bool cmdline_wants_help(const char *input) {
    if (!input)
        return false;

    while (*input) {
        while (*input == ' ')
            input++;

        const char *tok = input;
        while (*input && *input != ' ' && *input != '=')
            input++;

        size_t len = (size_t) (input - tok);
        if (len == 4 && memcmp(tok, "help", 4) == 0 && *input != '=')
            return true;

        /* skip the rest of this token (any =value and trailing chars) */
        while (*input && *input != ' ')
            input++;
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
        while (*input == ' ')
            input++;

        const char *var_start = input;
        while (*input && *input != '=' && *input != ' ')
            input++;

        const char *var_end = input;

        while (var_end > var_start && *(var_end - 1) == ' ')
            var_end--;

        while (*input && *input != '=')
            input++;

        if (*input != '=')
            break;

        input++;

        while (*input == ' ')
            input++;

        const char *val_start = input;
        while (*input && *input != ' ')
            input++;

        const char *val_end = input;

        uint64_t var_len = var_end - var_start;
        if (var_len >= MAX_VAR_LEN)
            var_len = MAX_VAR_LEN - 1;

        memcpy(var_buf, var_start, var_len);
        var_buf[var_len] = '\0';

        uint64_t val_len = val_end - val_start;
        if (val_len >= MAX_VAL_LEN)
            val_len = MAX_VAL_LEN - 1;

        memcpy(val_buf, val_start, val_len);
        val_buf[val_len] = '\0';

        cmdline_dispatch(var_buf, val_buf);
    }

    cmdline_apply_defaults();
    cmdline_check_for_unfilled();
    cmdline_print_all();
}
