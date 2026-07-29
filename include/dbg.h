/* @title: Debugging */
#include <stddef.h>
#include <stdint.h>

#define STACK_TRACE_MAX_DEPTH 64

void debug_print_registers();

void debug_print_stack();

void debug_print_memory(void *addr, size_t size);

void debug_print_stack_from(uint64_t *start, size_t max_scan);

size_t stack_unwind(uint64_t frame, uint64_t *entries, size_t max);
void debug_print_stack_trace(const uint64_t *entries, size_t nr);

/* Unwinds from the caller's own frame */
#define stack_trace_save(entries, max)                                         \
    stack_unwind((uint64_t) __builtin_frame_address(0), (entries), (max))
#pragma once
