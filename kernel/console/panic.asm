global panic_entry
extern panic_handler

; fill `struct panic_regs` on stack and give to panic_handler()
; the first push is at the highest offset, so we go backwards here

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

    mov rax, cr3
    push rax
    mov rax, cr2
    push rax

    pushfq

    ; 19 pushes of 8 below rsp should be the caller's next insn
    push qword [rsp + 152]

    ; Caller's rsp is above its return address, 160 of frame, 8 of return
    lea rax, [rsp + 168]
    mov [rsp + 152], rax

    mov rdi, rsp
    call panic_handler

    add rsp, 160
    ret
