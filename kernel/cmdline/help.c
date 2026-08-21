#include "internal.h"

static void validate_default_choice_or_mapping(const struct cmdline_entry *e,
                                               const char *name) {
    if (e->choices && e->default_val) {
        if (!cmdline_has_choice(e->choices, e->default_val))
            panic("cmdline entry '%s' default value '%s' is not in "
                  "choices list",
                  name, e->default_val);
    }

    if (e->mappings && e->default_val) {
        uint64_t mapped_val = 0;
        if (!cmdline_lookup_mapping(e->mappings, e->default_val, &mapped_val))
            panic("cmdline entry '%s' default value '%s' is not in "
                  "mappings list",
                  name, e->default_val);
    }

    if (e->flags_table && e->default_val) {
        uint64_t mask = 0;
        const char *err_tok = NULL;
        size_t err_len = 0;
        if (!cmdline_parse_flags(e->flags_table, e->default_val, &mask,
                                 &err_tok, &err_len))
            panic("cmdline entry '%s' default value '%s' contains invalid "
                  "flag '%.*s'",
                  name, e->default_val, (int) err_len, err_tok);
    }
}

static void validate_default_range(const struct cmdline_entry *e,
                                   const char *name) {
    if (!cmdline_entry_has_range(e) || !e->default_val)
        return;

    enum cmdline_type t = cmdline_entry_effective_type(e);

    if (t == CMDLINE_TYPE_FX) {
        fx32_32_t val = 0;
        if (!parse_is_fx(e->default_val, &val) ||
            val < (fx32_32_t) e->range.low || val > (fx32_32_t) e->range.hi)
            panic("cmdline entry '%s' default value '%s' out of range", name,
                  e->default_val);
    } else if (t == CMDLINE_TYPE_DURATION) {
        time_ns_t val = 0;
        if (!parse_is_duration(e->default_val, &val) ||
            !RANGE_CONTAINS(e->range, val))
            panic("cmdline entry '%s' default value '%s' out of range", name,
                  e->default_val);
    } else if (t == CMDLINE_TYPE_INT) {
        int64_t val = 0;
        if (!parse_is_int(e->default_val, &val) ||
            val < (int64_t) e->range.low || val > (int64_t) e->range.hi)
            panic("cmdline entry '%s' default value '%s' out of range", name,
                  e->default_val);
    } else if (t == CMDLINE_TYPE_UINT || t == CMDLINE_TYPE_DATA_SIZE) {
        uint64_t val = 0;
        if ((!parse_is_uint(e->default_val, &val) &&
             !parse_is_data_size(e->default_val, &val)) ||
            !RANGE_CONTAINS(e->range, val))
            panic("cmdline entry '%s' default value '%s' out of range", name,
                  e->default_val);
    }
}

void cmdline_validate_defaults(void) {
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        char name[CMDLINE_ENTRY_NAME_LEN_MAX];
        cmdline_functional_name(e, name);

        validate_default_choice_or_mapping(e, name);
        validate_default_range(e, name);
    }
}

bool cmdline_wants_help(const char *input) {
    cmdline_assign_all_args();

    const char *p = input;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;

        if (*p == '\0')
            break;

        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1) != '\0')
                    p += 2;
                else
                    p++;
            }
            if (*p == '"')
                p++;
            continue;
        }

        const char *word_start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;

        size_t len = p - word_start;
        if ((len == 4 && strncmp(word_start, "help", 4) == 0) ||
            (len == 6 && strncmp(word_start, "--help", 6) == 0) ||
            (len == 2 && strncmp(word_start, "-h", 2) == 0))
            return true;
    }
    return false;
}

static void print_help_range(const struct cmdline_entry *e) {
    if (!cmdline_entry_has_range(e))
        return;

    enum cmdline_type t = cmdline_entry_effective_type(e);

    if (t == CMDLINE_TYPE_FX) {
        int64_t lo_whole = ((int64_t) e->range.low) >> 32;
        uint64_t lo_frac = ((e->range.low & 0xFFFFFFFFULL) * 100) >> 32;
        int64_t hi_whole = ((int64_t) e->range.hi) >> 32;
        uint64_t hi_frac = ((e->range.hi & 0xFFFFFFFFULL) * 100) >> 32;
        printf(", range: [%lld.%02llu..%lld.%02llu]", (long long) lo_whole,
               (unsigned long long) lo_frac, (long long) hi_whole,
               (unsigned long long) hi_frac);
    } else if (t == CMDLINE_TYPE_DURATION) {
        if (e->range.hi >= 1000000000ULL && e->range.hi % 1000000000ULL == 0)
            printf(", range: [%lluns..%llus]",
                   (unsigned long long) e->range.low,
                   (unsigned long long) (e->range.hi / 1000000000ULL));
        else if (e->range.hi >= 1000000ULL && e->range.hi % 1000000ULL == 0)
            printf(", range: [%lluns..%llums]",
                   (unsigned long long) e->range.low,
                   (unsigned long long) (e->range.hi / 1000000ULL));
        else if (e->range.hi >= 1000ULL && e->range.hi % 1000ULL == 0)
            printf(", range: [%lluns..%lluus]",
                   (unsigned long long) e->range.low,
                   (unsigned long long) (e->range.hi / 1000ULL));
        else
            printf(", range: [%lluns..%lluns]",
                   (unsigned long long) e->range.low,
                   (unsigned long long) e->range.hi);
    } else if (t == CMDLINE_TYPE_INT) {
        printf(", range: [%lld..%lld]", (long long) (int64_t) e->range.low,
               (long long) (int64_t) e->range.hi);
    } else if (t == CMDLINE_TYPE_UINT || t == CMDLINE_TYPE_DATA_SIZE) {
        printf(", range: [%llu..%llu]", (unsigned long long) e->range.low,
               (unsigned long long) e->range.hi);
    }
}

static void print_help_metadata(const struct cmdline_entry *e) {
    if (e->choices) {
        printf(", choices: [");
        for (const char *const *c = e->choices; *c != NULL; c++) {
            printf("%s%s", (c == e->choices) ? "" : ", ", *c);
        }
        printf("]");
    } else if (e->mappings) {
        printf(", choices: [");
        for (const struct cmdline_map *m = e->mappings; m->name != NULL; m++) {
            printf("%s%s", (m == e->mappings) ? "" : ", ", m->name);
        }
        printf("]");
    }

    if (e->flags_table) {
        printf(", flags: [");
        for (const struct cmdline_flag *f = e->flags_table; f->name != NULL;
             f++) {
            printf("%s%s", (f == e->flags_table) ? "" : "|", f->name);
        }
        printf("]");
    }
}

__noreturn void cmdline_dump_help(void) {
    printf("charmos kernel command-line options:\n\n");
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        if (e->flags & (CMDLINE_ENTRY_HIDDEN | CMDLINE_ENTRY_SYMBOLIC))
            continue;

        char name[CMDLINE_ENTRY_NAME_LEN_MAX];
        cmdline_functional_name(e, name);
        printf("  %s%s%s\n", name, e->arg ? "=" : "", e->arg ? e->arg : "");
        if (e->desc)
            printf("      %s\n", e->desc);
        printf("      %s",
               (e->flags & CMDLINE_ENTRY_REQUIRED) ? "required" : "optional");
        if (e->default_val)
            printf(", default: %s", e->default_val);

        print_help_range(e);
        print_help_metadata(e);
        printf("\n\n");
    }

    if (__skernel_cmdline_schemas < __ekernel_cmdline_schemas) {
        printf("subsystem schemas:\n\n");
        for (struct cmdline_schema *s = __skernel_cmdline_schemas;
             s < __ekernel_cmdline_schemas; s++) {
            const char *hint = s->path_hint ? s->path_hint : "<path>";
            printf("  %s.%s.<property>\n", s->prefix, hint);
            if (s->desc)
                printf("      %s\n", s->desc);
            for (size_t i = 0; i < s->prop_count; i++) {
                const struct cmdline_schema_prop *p = &s->props[i];
                printf("      .%s (%s)", p->name,
                       p->parse ? "custom" : type_enum_str(p->c_type));
                if (p->desc)
                    printf(" - %s", p->desc);
                printf("\n");
            }
            printf("\n");
        }
    }

    printf("(kernel halted after `help`)\n");

    disable_interrupts();
    for (;;)
        hcf();
}
