.section .text
.global isr_trampolines

.macro ISR_STUB n
.global irq\n\()_entry
.type irq\n\()_entry, @function
irq\n\()_entry:
    pushq $\n
    jmp irq_dispatch
.endm

.rept 224
ISR_STUB %0
.set %0, %0 + 1
.endr

.global irq_dispatch
.type irq_dispatch, @function
irq_dispatch:
    popq %rdi   
    call irq_common_handler
    iretq

