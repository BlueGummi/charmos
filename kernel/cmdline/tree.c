#include "internal.h"

#define CMDLINE_MAX_TREE_DEPTH 16

void cmdline_functional_name(const struct cmdline_entry *ent,
                             char name_out[CMDLINE_ENTRY_NAME_LEN_MAX]) {
    const struct cmdline_entry *chain[CMDLINE_MAX_TREE_DEPTH];
    size_t depth = 0;

    for (const struct cmdline_entry *curr = ent; curr != NULL;
         curr = curr->parent) {
        kassert(depth < CMDLINE_MAX_TREE_DEPTH);
        chain[depth++] = curr;
    }

    size_t idx = 0;
    for (size_t i = depth; i > 0; i--) {
        const struct cmdline_entry *node = chain[i - 1];
        size_t len = strlen(node->name);
        if (idx + len >= CMDLINE_ENTRY_NAME_LEN_MAX - 1)
            len = (CMDLINE_ENTRY_NAME_LEN_MAX - 1 > idx)
                      ? (CMDLINE_ENTRY_NAME_LEN_MAX - 1 - idx)
                      : 0;

        memcpy(name_out + idx, node->name, len);
        idx += len;

        if (i > 1 && idx < CMDLINE_ENTRY_NAME_LEN_MAX - 1)
            name_out[idx++] = '.';
    }
    name_out[idx] = '\0';
}

void cmdline_check_for_duplicates(void) {
    size_t count =
        (size_t) (__ekernel_cmdline_entries - __skernel_cmdline_entries);
    if (count == 0)
        return;

    char (*names)[CMDLINE_ENTRY_NAME_LEN_MAX] =
        kmalloc_or_die(count * sizeof(*names));

    for (size_t i = 0; i < count; i++) {
        cmdline_functional_name(&__skernel_cmdline_entries[i], names[i]);
    }

    size_t dupes = 0;
    for (size_t i = 0; i < count; i++) {
        const char *name_a = names[i];

        for (size_t j = i + 1; j < count; j++) {
            const char *name_b = names[j];

            if (strcmp(name_a, name_b) == 0) {
                log_msg(LOG_ERROR, "duplicate command line entry: %s", name_a);
                dupes++;
            }
        }
    }

    kfree(names);

    if (dupes)
        panic("%zu duplicate command line entries", dupes);
}

void cmdline_check_for_unfilled(void) {
    size_t found = 0;
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        if ((e->flags & CMDLINE_ENTRY_REQUIRED) &&
            e->status == CMDLINE_ENTRY_NOT_FOUND) {
            char name[CMDLINE_ENTRY_NAME_LEN_MAX];
            cmdline_functional_name(e, name);
            log_msg(LOG_ERROR, "Required command line entry %s not present",
                    name);
            found++;
        }
    }
    if (found)
        panic("%zu required command line entries not present", found);
}

void cmdline_apply_defaults(void) {
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        if (e->status == CMDLINE_ENTRY_NOT_FOUND && e->default_val) {
            char name[CMDLINE_ENTRY_NAME_LEN_MAX];
            cmdline_functional_name(e, name);

            dispatch_parse_value(e, name, name, e->default_val);
            e->status = CMDLINE_ENTRY_DEFAULTED;
            log_msg(LOG_INFO, "command line entry '%s' defaulted to '%s'", name,
                    e->default_val);
        }
    }
}

void cmdline_print_all(void) {
#ifdef DEBUG_CMDLINE
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        if (e->flags & CMDLINE_ENTRY_SYMBOLIC)
            continue;

        char name[CMDLINE_ENTRY_NAME_LEN_MAX];
        cmdline_functional_name(e, name);
        log_msg(LOG_INFO, "command line entry %s", name);
    }
#endif
}
