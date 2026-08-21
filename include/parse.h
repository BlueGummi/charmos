/* @title: Parsing */
#pragma once
#include <math/fixed.h>
#include <smp/topology.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <types/types.h>

struct cmdline_range;

/* Just a container for the list */
struct parse_list {
    size_t count;
    char **items; /* Dynamically allocated array of null terminated strings */
};

/* Returns true if the string matches the type,
 * and stores parsed value in *out */
bool parse_is_bool(const char *str, bool *out);
bool parse_is_data_size(const char *str, uint64_t *out);
bool parse_is_duration(const char *str, time_ns_t *out);
bool parse_is_cpu_mask(const char *str, struct cpu_mask *out, size_t n_cpus);
bool parse_is_mac(const char *str, uint64_t *out);
bool parse_is_fx(const char *str, fx32_32_t *out);
bool parse_is_int(const char *str, int64_t *out);
bool parse_is_uint(const char *str, uint64_t *out);
bool parse_is_range(const char *str, uint64_t *start, uint64_t *end);
bool parse_is_list(const char *str, struct parse_list *out);

void parse_list_free(struct parse_list *list);

#define parse_list_for_each(item_var, list)                                    \
    for (size_t __i = 0;                                                       \
         __i < (list)->count && ((item_var = (list)->items[__i]), true);       \
         __i++)
