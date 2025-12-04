; void switch_context(struct context *old, struct context *new)
global switch_context
switch_context:
    ; Save callee-saved regs into *old
    mov [rdi + 0x00], rbx
    mov [rdi + 0x08], rbp
    mov [rdi + 0x10], r12
    mov [rdi + 0x18], r13
    mov [rdi + 0x20], r14
    mov [rdi + 0x28], r15
    mov [rdi + 0x30], rsp
    lea rax, [rel .Lreturn_point]
    mov [rdi + 0x38], rax

    ; Load callee-saved regs from *new
    mov rbx, [rsi + 0x00]
    mov rbp, [rsi + 0x08]
    mov r12, [rsi + 0x10]
    mov r13, [rsi + 0x18]
    mov r14, [rsi + 0x20]
    mov r15, [rsi + 0x28]
    mov rsp, [rsi + 0x30]
    jmp [rsi + 0x38]     ; jump to saved RIP


.Lreturn_point:
    ret


global load_context
; void load_context(struct context *new)
load_context:
    mov rsi, rdi              ; rdi holds pointer to new context

    mov rbx, [rsi + 0x00]
    mov rbp, [rsi + 0x08]
    mov r12, [rsi + 0x10]
    mov r13, [rsi + 0x18]
    mov r14, [rsi + 0x20]
    mov r15, [rsi + 0x28]
    mov rsp, [rsi + 0x30]
    jmp [rsi + 0x38]          ; jump to saved RIP (entry point)

; void save_context(struct context *ctx)
global save_context
save_context:
    ; rdi = ctx pointer

    ; Save callee-saved registers
    mov [rdi + 0x00], rbx
    mov [rdi + 0x08], rbp
    mov [rdi + 0x10], r12
    mov [rdi + 0x18], r13
    mov [rdi + 0x20], r14
    mov [rdi + 0x28], r15
    mov [rdi + 0x30], rsp

    ; Save return RIP using call/pop trick
    lea rax, [rel .Lsaved_rip]   ; the RIP after returning
    mov [rdi + 0x38], rax

.Lsaved_rip:
    ret

