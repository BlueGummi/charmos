/* @title: Profiling */
#pragma once
#include <compiler/core.h>
#include <global.h>
#include <linker/symbols.h>
#include <stdbool.h>
#include <stdint.h>
#include <structures/list.h>

struct profiling_entry {
    const char *name;
    void *data;
    const char *(*to_str)(void *data);
    void (*log)(void *data);
    struct list_node list_node;
};

/* Current set of profiling flags:
 *
 * PROFILING_ALL - Enables all profiling
 * PROFILING_SCHED - Enables scheduler profiling
 *
 * TODO: more...
 */

/* Initialize profiling */
void profiling_init(void);
void profiling_log_all(void);

#define PROFILE_SECTION LINKER_SECTION_ATTRIBUTE(profiling_data)

#define REGISTER_PROFILING_ENTRY(entry)                                        \
    static const struct profiling_entry entry PROFILE_SECTION
