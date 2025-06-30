global syscall_entry
extern syscall_handler

syscall_entry:
    push rax
    push rbx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r12
    push r13
    push r14
    push r15
    push rcx
    push r11

    ; rax = syscall number
    ; rdi = arg1, rsi = arg2, rdx = arg3, r10 = arg4, r8 = arg5, r9 = arg6

    mov r12, rdi
    mov r13, rsi
    mov r14, rdx
    mov r15, r10
    mov rbx, r8

    mov rdi, rax   
    mov rsi, r12
    mov rdx, r13
    mov rcx, r14   
    mov r8,  r15
    mov r9,  rbx

    call syscall_handler

    pop r11
    pop rcx
    pop r15
    pop r14
    pop r13
    pop r12
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rbx
    pop rax

    o64 sysret
