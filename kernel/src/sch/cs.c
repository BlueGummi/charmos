#include <io.h>
#include <sched.h>
#include <task.h>

__attribute__((naked)) void timer_interrupt_handler(void) {
    asm volatile("push %rax\n\t"
                 "push %rbx\n\t"
                 "push %rcx\n\t"
                 "push %rdx\n\t"
                 "push %rsi\n\t"
                 "push %rdi\n\t"
                 "push %rbp\n\t"
                 "push %r8\n\t"
                 "push %r9\n\t"
                 "push %r10\n\t"
                 "push %r11\n\t"
                 "push %r12\n\t"
                 "push %r13\n\t"
                 "push %r14\n\t"
                 "push %r15\n\t"

                 "mov %rsp, %rdi\n\t"
                 "call schedule\n\t"

                 "mov $0x20, %al\n\t"
                 "out %al, $0x20\n\t"

                 "pop %r15\n\t"
                 "pop %r14\n\t"
                 "pop %r13\n\t"
                 "pop %r12\n\t"
                 "pop %r11\n\t"
                 "pop %r10\n\t"
                 "pop %r9\n\t"
                 "pop %r8\n\t"
                 "pop %rbp\n\t"
                 "pop %rdi\n\t"
                 "pop %rsi\n\t"
                 "pop %rdx\n\t"
                 "pop %rcx\n\t"
                 "pop %rbx\n\t"
                 "pop %rax\n\t"

                 "iretq\n\t");
}
