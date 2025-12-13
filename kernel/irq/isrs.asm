extern isr_common_entry
section .text

%macro ISR_STUB 1
global isr_vector_%1
isr_vector_%1:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rsi
    push rdi
    push rbp
    push rdx
    push rcx
    push rbx
    push rax

    mov rdi, %1
    mov rsi, rsp
    call isr_common_entry

    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rbp
    pop rdi
    pop rsi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    iretq
%endmacro

%assign i 0
%rep 256
    ISR_STUB i
%assign i i+1
%endrep

