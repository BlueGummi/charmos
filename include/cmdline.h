/* @title: Command Line */
#pragma once
#include <compiler.h>
#include <linker/symbols.h>
#include <stdbool.h>

enum cmdline_status {
    CMDLINE_NOT_FOUND = 0,
    CMDLINE_DEFAULTED,
    CMDLINE_FOUND,
};

typedef void (*cmdline_callback)(const char *value);

struct cmdline_entry {
    const char *name;
    const char *desc; /* human-readable description, for docs & help */
    const char *arg;  /* value format hint, e.g. "<hex bytes>", "<device>" */
    cmdline_callback callback;
    char **value;
    const char *default_val;
    enum cmdline_status status;
    bool required;
};

#define CMDLINE_ENTRY_DECLARE(n, ...)                                          \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    __cmdline_##n = {.name = #n, .status = CMDLINE_NOT_FOUND, __VA_ARGS__}

LINKER_SECTION_DEFINE(struct cmdline_entry, cmdline_entries);

void cmdline_parse(const char *input);
bool cmdline_wants_help(const char *input);
__noreturn void cmdline_dump_help(void);
