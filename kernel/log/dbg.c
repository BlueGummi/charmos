#include <console/printf.h>
#include <stddef.h>
#include <stdint.h>

void debug_print_registers() {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rsp, rbp, rip, rflags;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

    __asm__ volatile("movq %%rax, %0\n\t"
                     "movq %%rbx, %1\n\t"
                     "movq %%rcx, %2\n\t"
                     "movq %%rdx, %3\n\t"
                     "movq %%rsi, %4\n\t"
                     "movq %%rdi, %5\n\t"
                     "movq %%rsp, %6\n\t"
                     "movq %%rbp, %7\n\t"
                     "leaq (%%rip), %8\n\t"
                     "pushfq\n\t"
                     "popq %9\n\t"
                     : "=r"(rax), "=r"(rbx), "=r"(rcx), "=r"(rdx), "=r"(rsi),
                       "=r"(rdi), "=r"(rsp), "=r"(rbp), "=r"(rip), "=r"(rflags)
                     :
                     : "memory");

    __asm__ volatile("movq %%r8, %0\n\t"
                     "movq %%r9, %1\n\t"
                     "movq %%r10, %2\n\t"
                     "movq %%r11, %3\n\t"
                     "movq %%r12, %4\n\t"
                     "movq %%r13, %5\n\t"
                     "movq %%r14, %6\n\t"
                     "movq %%r15, %7\n\t"
                     : "=r"(r8), "=r"(r9), "=r"(r10), "=r"(r11), "=r"(r12),
                       "=r"(r13), "=r"(r14), "=r"(r15)
                     :
                     : "memory");

    k_printf("Registers:\n");
    k_printf("   RAX: 0x%lx\n", rax);
    k_printf("   RBX: 0x%lx\n", rbx);
    k_printf("   RCX: 0x%lx\n", rcx);
    k_printf("   RDX: 0x%lx\n", rdx);
    k_printf("   RSI: 0x%lx\n", rsi);
    k_printf("   RDI: 0x%lx\n", rdi);
    k_printf("   RSP: 0x%lx\n", rsp);
    k_printf("   RBP: 0x%lx\n", rbp);
    k_printf("   RIP: 0x%lx\n", rip);
    k_printf("   RFLAGS: 0x%lx\n", rflags);
    k_printf("   R8: 0x%lx\n", r8);
    k_printf("   R9: 0x%lx\n", r9);
    k_printf("   R10: 0x%lx\n", r10);
    k_printf("   R11: 0x%lx\n", r11);
    k_printf("   R12: 0x%lx\n", r12);
    k_printf("   R13: 0x%lx\n", r13);
    k_printf("   R14: 0x%lx\n", r14);
    k_printf("   R15: 0x%lx\n", r15);
}

void debug_print_stack() {
    uint64_t *rsp;

    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));

    k_printf("Stack (from rsp):\n");
    for (int i = 0; i < 32; i++) {
        if (rsp[i] != 0)
            k_printf("  [%2d] 0x%016lx\n", i, rsp[i]);
    }
}
void debug_print_memory(void *addr, size_t size) {
    uint8_t *ptr = (uint8_t *) addr;
    k_printf("Memory at 0x%lx:\n", (uint64_t) addr);
    for (size_t i = 0; i < size; i++) {
        if (i % 16 == 0) {
            if (i != 0)
                k_printf("\n");
            k_printf("0x%lx: ", (uint64_t) (ptr + i));
        }
        k_printf("%02x ", ptr[i]);
    }
    k_printf("\n");
}
