#include <console/printf.h>
#include <fs/vfs.h>
#include <mem/alloc.h>
#include <string.h>

#define MAX_VAR_LEN 128
#define MAX_VAL_LEN 256

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

        size_t var_len = var_end - var_start;
        if (var_len >= MAX_VAR_LEN)
            var_len = MAX_VAR_LEN - 1;
        memcpy(var_buf, var_start, var_len);
        var_buf[var_len] = '\0';

        size_t val_len = val_end - val_start;
        if (val_len >= MAX_VAL_LEN)
            val_len = MAX_VAL_LEN - 1;
        memcpy(val_buf, val_start, val_len);
        val_buf[val_len] = '\0';

        if (strcmp(var_buf, "root") == 0) {
            char *val = kmalloc(strlen(val_buf));
            memcpy(val, val_buf, strlen(val_buf));
            g_root_part = val;
        }
    }
}
