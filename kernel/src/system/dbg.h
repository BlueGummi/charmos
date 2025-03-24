#include <stddef.h>
#include <stdint.h>

void debug_print_registers();

void debug_print_stack(int num_frames);

void debug_print_memory(void *addr, size_t size);
