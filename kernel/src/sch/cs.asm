extern schedule
global timer_interrupt_handler
timer_interrupt_handler:
    ; Push all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Save segment registers, rip, rflags, rsp, etc, already pushed by CPU
    mov rdi, rsp   ; Pass pointer to cpu_state_t to C
    call schedule
    mov dx, 0x20
    mov ax, 0x20
    out dx, ax
    ; Pop all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ; Return from interrupt
    iretq

