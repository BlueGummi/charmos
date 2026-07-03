#include <asm.h>
#include <cmdline.h>
#include <console/panic.h>
#include <console/printf.h>
#include <fs/vfs.h>
#include <global.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <string.h>

#define MAX_VAR_LEN 128
#define MAX_VAL_LEN 256

CMDLINE_ENTRY_DECLARE(root,
                      .desc = "Root filesystem partition to mount at boot",
                      .arg = "<device>", .default_val = NULL,
                      .value = &global.root_partition, .required = true);

static void cmdline_check_for_unfilled(void) {
    size_t found = 0;
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        if (e->required && e->status == CMDLINE_NOT_FOUND) {
            log_msg(LOG_ERROR, "Required command line entry %s not present",
                    e->name);
            found++;
        }
    }
    if (found)
        panic("%zu required command line entries not present", found);
}

static void cmdline_apply_defaults(void) {
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        if (e->status == CMDLINE_NOT_FOUND && e->default_val) {
            if (e->value)
                *e->value = (char *) e->default_val;
            else if (e->callback)
                e->callback(e->default_val);
            e->status = CMDLINE_DEFAULTED;
            log_msg(LOG_INFO, "command line entry '%s' defauled to '%s'",
                    e->name, e->default_val);
        }
    }
}

static void cmdline_dispatch(const char *var, const char *val) {
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        kassert(e->name);
        if (strcmp(e->name, var) != 0)
            continue;

        if (e->status == CMDLINE_FOUND)
            panic("duplicate cmdline entry: %s", var);

        e->status = CMDLINE_FOUND;
        if (e->callback) {
            e->callback(val);
        } else if (e->value) {
            char *copy = alloc_or_die(kmalloc(strlen(val) + 1));
            memcpy(copy, val, strlen(val) + 1);
            *e->value = copy;
            log_msg(LOG_INFO, "command line entry '%s' set to '%s'", e->name,
                    copy);
        }
        return;
    }
    log_msg(LOG_WARN, "unknown key '%s', ignoring\n", var);
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
        printf("  %s%s%s\n", e->name, e->arg ? "=" : "", e->arg ? e->arg : "");
        if (e->desc)
            printf("      %s\n", e->desc);
        printf("      %s", e->required ? "required" : "optional");
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
}
