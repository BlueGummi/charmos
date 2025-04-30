#include <printf.h>
#include <stdalign.h>
#include <stdint.h>
#include <gdt.h>

alignas(8) struct gdt_entry gdt[3];
struct gdt_ptr gp;

void gdt_set_gate(int num, uint64_t base, uint32_t limit, uint8_t access,
                  uint8_t gran) {
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].access = access;
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].base_high = (base >> 24) & 0xFF;
}

void gdt_install() {
    asm volatile("cli\n\t");

    gp.limit = (sizeof(struct gdt_entry) * 3) - 1;
    gp.base = (uint64_t) &gdt;

    // Null descriptor
    gdt_set_gate(0, 0, 0, 0, 0);

    // CS
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF); // L bit set (0xAF)

    // Data segment descriptor
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    asm volatile("lgdt %0" : : "m"(gp));

    asm volatile(".intel_syntax noprefix\n\t"
                 "lea rax, [0x8]\n\t"
                 "push rax\n\t"
                 "lea rax, [rip + this]\n\t"
                 "push rax\n\t"
                 "retfq\n\t"
                 "this:\n\t"
                 "mov ax, 0x10\n\t"
                 "mov ds, ax\n\t"
                 "mov es, ax\n\t"
                 "mov fs, ax\n\t"
                 "mov gs, ax\n\t"
                 "mov ss, ax\n\t"
                 ".att_syntax prefix\n\t"
                 :
                 :
                 : "rax", "ax", "memory");
}
