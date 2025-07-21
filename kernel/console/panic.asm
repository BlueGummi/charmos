global panic_entry
extern panic_handler
panic_entry:
    push rsp        
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

    mov rdi, rsp         
    call panic_handler

    add rsp, 128
    ret
