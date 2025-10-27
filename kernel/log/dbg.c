#include <console/printf.h>
#include <misc/syms.h>
#include <sch/sched.h>
#include <stddef.h>
#include <stdint.h>

const char *find_symbol(uint64_t addr, uint64_t *out_sym_addr) {
    const char *result = NULL;
    uint64_t best = 0;

    for (uint64_t i = 0; i < syms_len; i++) {
        if (syms[i].addr <= addr && syms[i].addr > best) {
            best = syms[i].addr;
            result = syms[i].name;
        }
    }

    if (out_sym_addr)
        *out_sym_addr = best;

    return result;
}

void debug_print_stack(void) {
    uint64_t *stack_top;

    uint64_t *rbp, *rsp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    asm volatile("mov %%rsp, %0" : "=r"(rsp));

    stack_top = (void *) ((uint8_t *) scheduler_get_current_thread()->stack +
                          scheduler_get_current_thread()->stack_size);

    int hits = 0;
    for (uint64_t *p = rsp; p < stack_top; p++) {
        uint64_t val = *p;

        if (val >= 0xffffffff80000000ULL && val <= 0xffffffffffffffffULL) {
            uint64_t sym_addr;
            const char *sym = find_symbol(val, &sym_addr);
            if (sym) {
                k_printf("    [0x%016lx] %s+0x%lx (sp=0x%016lx)\n", val, sym,
                         val - sym_addr, (uint64_t) p);
                hits++;
            }
        }
    }

    if (hits == 0)
        k_printf("  <no kernel symbols found>\n");
}

void debug_print_memory(void *addr, uint64_t size) {
    uint8_t *ptr = (uint8_t *) addr;
    k_printf("Memory at 0x%lx:\n", (uint64_t) addr);
    for (uint64_t i = 0; i < size; i++) {
        if (i % 16 == 0) {
            if (i != 0)
                k_printf("\n");
            k_printf("0x%lx: ", (uint64_t) (ptr + i));
        }
        k_printf("%02x ", ptr[i]);
    }
    k_printf("\n");
}
