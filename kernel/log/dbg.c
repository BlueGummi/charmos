#include <console/printf.h>
#include <misc/syms.h>
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

void debug_print_stack() {
    uint64_t *rbp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));

    int frame = 0;
    while (rbp) {
        uint64_t ret_addr = rbp[1];
        if (ret_addr >= 0xffffffff80000000) {
            uint64_t sym_addr;
            const char *sym = find_symbol(ret_addr, &sym_addr);
            if (sym) {
                k_printf("    [%d] 0x%016lx <%s+0x%lx>\n", frame, ret_addr, sym,
                         ret_addr - sym_addr);
            } else {
                k_printf("    [%d] 0x%016lx <unknown>\n", frame, ret_addr);
            }
        }
        rbp = (uint64_t *) rbp[0];
        frame++;
    }
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
