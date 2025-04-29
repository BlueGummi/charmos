extern page_fault_handler

global page_fault_handler_wrapper
page_fault_handler_wrapper:
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

    ; Extract error code (stored at [rsp + 120] due to pushes above)
    mov rdi, [rsp + 120]  ; Error code (1st argument)
    mov rsi, cr2          ; Faulting address (2nd argument)
    call page_fault_handler

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

    add rsp, 8

    iretq
