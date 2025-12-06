/* @title: Profiling */
#pragma once
#include <charmos.h>
#include <compiler.h>
#include <linker/symbols.h>
#include <stdbool.h>
#include <stdint.h>
#include <structures/list.h>

struct profiling_entry {
    const char *name;
    void *data;
    const char *(*to_str)(void *data);
    void (*log)(void *data);
    struct list_head list_node;
} __linker_aligned;

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

#define PROFILE_SECTION __attribute__((section(".kernel_profiling_data"), used))

#define REGISTER_PROFILING_ENTRY(entry)                                        \
    static const struct profiling_entry entry PROFILE_SECTION
