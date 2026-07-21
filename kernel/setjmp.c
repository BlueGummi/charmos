#include <setjmp.h>
#include <stdint.h>

/*
 * Offsets:
 * 0: RBX
 * 1: RBP
 * 2: R12
 * 3: R13
 * 4: R14
 * 5: R15
 * 6: RSP
 * 7: RIP
 */

__naked int setjmp(jmp_buf env) {
    asm volatile("movq %rbx, (%rdi)\n\t"
                 "movq %rbp, 8(%rdi)\n\t"
                 "movq %r12, 16(%rdi)\n\t"
                 "movq %r13, 24(%rdi)\n\t"
                 "movq %r14, 32(%rdi)\n\t"
                 "movq %r15, 40(%rdi)\n\t"

                 "leaq 8(%rsp), %rax\n\t"
                 "movq %rax, 48(%rdi)\n\t"

                 "movq (%rsp), %rax\n\t"
                 "movq %rax, 56(%rdi)\n\t"

                 "xorq %rax, %rax\n\t"
                 "retq");
}

__naked void longjmp(jmp_buf env, int val) {
    asm volatile("movq (%rdi), %rbx\n\t"
                 "movq 8(%rdi), %rbp\n\t"
                 "movq 16(%rdi), %r12\n\t"
                 "movq 24(%rdi), %r13\n\t"
                 "movq 32(%rdi), %r14\n\t"
                 "movq 40(%rdi), %r15\n\t"

                 "movq 48(%rdi), %rsp\n\t"

                 "movq 56(%rdi), %rdx\n\t"

                 "movl %esi, %eax\n\t"
                 "testl %eax, %eax\n\t"
                 "jne 1f\n\t"
                 "movl $1, %eax\n\t"

                 "1:\n\t"
                 "jmp *%rdx\n\t");
}
