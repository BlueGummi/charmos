global panic_entry
global crash_capture_regs
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

; void crash_capture_regs(struct panic_regs *out);
; rdi = struct panic_regs*
crash_capture_regs:
    mov [rdi + 32], rax
    mov [rdi + 40], rbx
    mov [rdi + 48], rcx
    mov [rdi + 56], rdx
    mov [rdi + 64], rbp
    mov [rdi + 72], rdi
    mov [rdi + 80], rsi
    mov [rdi + 88], r8
    mov [rdi + 96], r9
    mov [rdi + 104], r10
    mov [rdi + 112], r11
    mov [rdi + 120], r12
    mov [rdi + 128], r13
    mov [rdi + 136], r14
    mov [rdi + 144], r15

    ; rip is return addr 
    mov rax, [rsp]
    mov [rdi + 0], rax

    ; rflags
    pushfq
    pop rax
    mov [rdi + 8], rax

    ; cr2, cr3
    mov rax, cr2
    mov [rdi + 16], rax
    mov rax, cr3
    mov [rdi + 24], rax

    ; rsp before call
    lea rax, [rsp + 8]
    mov [rdi + 152], rax

    ret
