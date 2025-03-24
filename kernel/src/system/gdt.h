#include <stdalign.h>
#include <stdint.h>

void gdt_set_gate(int num, uint64_t base, uint32_t limit, uint8_t access, uint8_t gran);

void gdt_install();
