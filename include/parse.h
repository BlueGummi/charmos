/* @title: Parsing */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <types/types.h>

ssize_t parse_csv(const char *input, char ***output);
bool parse_bool(const char *str);

/* Like 2M, 5G */
ssize_t parse_data_size(const char *str);
time_ns_t parse_duration(const char *str);
struct cpu_mask parse_cpu_mask(const char *str, size_t n_cpus);

#define MAC_INVALID 0xFFFFFFFFFFFFFFFFULL
uint64_t parse_mac(const char *str);
