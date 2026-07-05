/* @title: Command Line */
#pragma once
#include <compiler.h>
#include <linker/symbols.h>
#include <stdbool.h>
#include <stddef.h>

enum cmdline_status {
    CMDLINE_NOT_FOUND = 0,
    CMDLINE_DEFAULTED,
    CMDLINE_FOUND,
};

typedef void (*cmdline_callback)(const char *value);

/* The idea with parent-child relationships:
 *
 * The dot is used as a namespace. With a given cmdline_entry, we trace
 * its lineage to build out what name it *really* has.
 *
 * For instance, if we have
 *
 * apple->parent = orange
 * orange->parent = pineapple
 * pineapple->parent = NULL
 *
 * the "functional name" of apple is
 *
 * pineapple.orange.apple */
struct cmdline_entry {
    const char *name;
    const char *desc; /* human readable description */
    const char *arg;  /* value format hint e.g. "<hex bytes>", "<device>" */
    cmdline_callback callback;
    char **value;
    const char *default_val;
    enum cmdline_status status;
    bool required;
    struct cmdline_entry *parent;

    /* NOTE: we could keep track of children, but that's not at all mandatory */
};

#define CMDLINE_ENTRY_DECLARE(n, ...)                                          \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    __cmdline_##n = {.name = #n, .status = CMDLINE_NOT_FOUND, __VA_ARGS__}

#define CMDLINE_ENTRY(n, ...) &__cmdline_##n

#define CMDLINE_ENTRY_NAME_LEN_MAX 256

LINKER_SECTION_DEFINE(struct cmdline_entry, cmdline_entries);

void cmdline_parse(const char *input);
bool cmdline_wants_help(const char *input);
__noreturn void cmdline_dump_help(void);
