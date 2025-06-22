BITS 64
GLOBAL _start

SECTION .data
msg:    db "chat, is this ring 3?", 0xA, 0

SECTION .text
_start:
    mov rax, 1     
    lea rdi, [rel msg] 
    syscall

.hang:
    hlt
    jmp .hang

