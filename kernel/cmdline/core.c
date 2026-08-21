#include "internal.h"

static CMDLINE_DECLARE_STRING(
    root, global.root_partition,
    .desc = "Root filesystem partition to mount at boot", .arg = "<device>",
    .default_val = NULL, .flags = CMDLINE_ENTRY_REQUIRED);

static const char *parse_escaped_string(const char *input, char *buf,
                                        size_t max_len, bool in_quotes) {
    if (!buf || max_len == 0)
        return input;
    size_t idx = 0;

    while (*input) {
        if (*input == '\\' && *(input + 1) != '\0') {
            input++;
            char escaped = *input;
            switch (escaped) {
            case 'n': escaped = '\n'; break;
            case 't': escaped = '\t'; break;
            case 'r': escaped = '\r'; break;
            default: break;
            }
            if (idx < max_len - 1)
                buf[idx++] = escaped;
            input++;
            continue;
        }

        if (in_quotes) {
            if (*input == '"') {
                input++; /* consume closing quote */
                break;
            }
        } else {
            if (*input == ' ' || *input == '\t')
                break;
        }

        if (idx < max_len - 1)
            buf[idx++] = *input;
        input++;
    }

    buf[idx] = '\0';
    return input;
}

static const char *parse_next_token(const char *input,
                                    char var_buf[MAX_VAR_LEN],
                                    char val_buf[MAX_VAL_LEN]) {
    var_buf[0] = '\0';
    val_buf[0] = '\0';

    while (*input == ' ' || *input == '\t')
        input++;

    if (*input == '\0')
        return input;

    const char *var_start = input;
    while (*input && *input != '=' && *input != ' ' && *input != '\t')
        input++;

    if (*input != '=')
        return input;

    const char *var_end = input;
    input++; /* consume '=' */

    while (*input == ' ' || *input == '\t')
        input++;

    size_t var_len = (size_t) (var_end - var_start);
    if (var_len >= MAX_VAR_LEN)
        var_len = MAX_VAR_LEN - 1;
    memcpy(var_buf, var_start, var_len);
    var_buf[var_len] = '\0';

    bool is_quoted = (*input == '"');
    if (is_quoted)
        input++; /* consume opening quote */

    return parse_escaped_string(input, val_buf, MAX_VAL_LEN, is_quoted);
}

void cmdline_parse(const char *input) {
    char var_buf[MAX_VAR_LEN];
    char val_buf[MAX_VAL_LEN];

    cmdline_check_for_duplicates();
    cmdline_assign_all_args();
    cmdline_validate_defaults();

    while (*input) {
        input = parse_next_token(input, var_buf, val_buf);
        if (var_buf[0] != '\0')
            cmdline_dispatch(var_buf, val_buf);
    }

    cmdline_apply_defaults();
    cmdline_check_for_unfilled();
    cmdline_print_all();
}
