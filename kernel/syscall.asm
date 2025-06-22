global syscall_entry
extern syscall_handler
; void syscall_entry();
syscall_entry:
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
    push rdx
    push rcx
    push rbx
    push rax

    ; Arguments to syscall_handler:
    ; RAX = syscall num, RDI = arg1, RSI = arg2, RDX = arg3, R10 = arg4, R8 = arg5

    mov rdi, rax        ; syscall num
    mov rsi, rdi        ; arg1
    mov rdx, rsi        ; arg2
    mov rcx, rdx        ; arg3
    mov r8, r10         ; arg4
    mov r9, r8          ; arg5

    call syscall_handler

    pop rax
    pop rbx
    pop rcx
    pop rdx
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

    sysretq

