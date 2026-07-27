/* @title: Command Line */
#pragma once
#include <compiler.h>
#include <linker/symbols.h>
#include <stdbool.h>
#include <stddef.h>
#include <types/type_enum.h>

struct cmdline_entry;

enum cmdline_entry_status {
    CMDLINE_ENTRY_NOT_FOUND = 0,
    CMDLINE_ENTRY_DEFAULTED,
    CMDLINE_ENTRY_FOUND,
};

enum cmdline_entry_flags {
    CMDLINE_ENTRY_FLAGS_NONE = 0,
    CMDLINE_ENTRY_SYMBOLIC = 1, /* This entry serves parenting purposes,
                                 * however itself cannot be set to a value */
    CMDLINE_ENTRY_REQUIRED = 1 << 1,
    CMDLINE_ENTRY_DOCUMENTED = 1
                               << 2, /* If this is set, we print/document it */
};

typedef void (*cmdline_callback)(const char *value, struct cmdline_entry *ent);

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

    enum cmdline_entry_flags flags;
    enum cmdline_entry_status status;

    struct cmdline_entry *parent;

    /* Regarding the variable to write to */
    enum type_enum type;
    void *write_to;
    enum errno (*parse)(void *write_to, const char *text);

    void *private;

    /* NOTE: we could keep track of children, but that's not at all mandatory */
};

#define CMDLINE_ENTRY_DECLARE(n, ...)                                          \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    __cmdline_##n = {.name = #n,                                               \
                     .status = CMDLINE_ENTRY_NOT_FOUND,                        \
                     .type = TYPE_NONE,                                        \
                     .parse = NULL,                                            \
                     .flags = CMDLINE_ENTRY_FLAGS_NONE,                        \
                     __VA_ARGS__}

#define CMDLINE_ENTRY_DECLARE_TYPED(n, var, ...)                               \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    __cmdline_##n = {.name = #n,                                               \
                     .status = CMDLINE_ENTRY_NOT_FOUND,                        \
                     .write_to = &var,                                         \
                     .type = TYPE_TO_ENUM((var)),                              \
                     .flags = CMDLINE_ENTRY_FLAGS_NONE,                        \
                     __VA_ARGS__}

#define CMDLINE_ENTRY_DECLARE_TYPED_CUSTOM(n, var, pars, ...)                  \
    LINKER_SECTION_OBJECT(struct cmdline_entry, cmdline_entries)               \
    __cmdline_##n = {.name = #n,                                               \
                     .status = CMDLINE_ENTRY_NOT_FOUND,                        \
                     .write_to = &var,                                         \
                     .parse = pars,                                            \
                     .type = TYPE_NONE,                                        \
                     .flags = CMDLINE_ENTRY_FLAGS_NONE,                        \
                     __VA_ARGS__}

#define CMDLINE_ENTRY_DEFINE(n) extern struct cmdline_entry __cmdline_##n

#define CMDLINE_ENTRY(n, ...) &__cmdline_##n

#define CMDLINE_ENTRY_NAME_LEN_MAX 256

#define CMDLINE_ENTRY_TYPE_TO_ARG(type)                                        \
    _Generic((type) 0, int: "<integer>", bool: "<on/off>")

LINKER_SECTION_DEFINE(struct cmdline_entry, cmdline_entries);

extern enum errno (*cmdline_parse_table[TYPE_MAX])(void *write_to,
                                                   const char *text);

enum errno cmdline_parse_fx(void *write_to, const char *text);

/* Returns true if the str matches a 'yes' string, and false if it is a 'no',
 * although it's a bit more nuanced than that and matches a lot of things.
 *
 * panics if neither match */
bool cmdline_is_enabled(const char *str);
void cmdline_parse(const char *input);
bool cmdline_wants_help(const char *input);
__noreturn void cmdline_dump_help(void);
