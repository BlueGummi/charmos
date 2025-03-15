bits 32

section .multiboot
        align 4
        dd 0x1BADB002 
        dd 0x00 
        dd - (0x1BADB002 + 0x00) 

section .text
global start
extern k_main

start:
        cli ; no more interrupts :(

        ; Global Load Table Descriptor
        lgdt [gdt_descriptor]

        ; A20 line (the 21st bit of a memory access)
        in al, 0x92
        or al, 2
        out 0x92, al

        mov eax, cr0
        or eax, 0x1                 ; PE (Protection Enable) bit
        mov cr0, eax

        ; jump to flush the CPU pipeline and load CS with the new GDT
        jmp 0x08:protected_mode     ; 0x08 is the offset of the code segment in the GDT

protected_mode:
        mov ax, 0x10                ; 0x10 is the offset of the data segment in the GDT
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax

        mov esp, stack_top

        call k_main

        hlt

section .bss
align 16
stack_bottom:
        resb 16384                  ; 16kb stack (tiny)
stack_top:

section .data
gdt_start:
        dd 0x00000000               ; Null
        dd 0x00000000

        ; Code segment
        dd 0x0000FFFF               ; Limit low, Base low
        dd 0x00CF9A00               ; Flags, Limit high, Base high

        ; Data segment 
        dd 0x0000FFFF               ; Limit low, Base low
        dd 0x00CF9200               ; Flags, Limit high, Base high

gdt_end:

gdt_descriptor:
        dw gdt_end - gdt_start - 1  ; Limit (size of GDT - 1)
        dd gdt_start                ; Base address of GDT
