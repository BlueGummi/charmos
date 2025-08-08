#include <boot/gdt.h>
#include <boot/tss.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <stdint.h>
#include <string.h>

#define GDT_ENTRIES 7

void gdt_set_tss(struct gdt_entry_tss *tss_desc, uint64_t base,
                 uint32_t limit) {
    tss_desc->limit_low = limit & 0xFFFF;
    tss_desc->base_low = base & 0xFFFF;
    tss_desc->base_middle = (base >> 16) & 0xFF;
    tss_desc->access = 0x89; // Present, type = 64-bit TSS (available)
    tss_desc->granularity = (limit >> 16) & 0x0F;
    tss_desc->granularity |= 0x00; // No granularity for TSS
    tss_desc->base_high = (base >> 24) & 0xFF;
    tss_desc->base_upper = (base >> 32) & 0xFFFFFFFF;
    tss_desc->reserved = 0;
}

void gdt_set_gate(struct gdt_entry *gdt, int num, uint64_t base, uint32_t limit,
                  uint8_t access, uint8_t gran) {
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].access = access;
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].base_high = (base >> 24) & 0xFF;
}

void gdt_load(struct gdt_entry *gdt, uint64_t n_entries) {
    struct gdt_ptr gp = {
        .limit = (sizeof(struct gdt_entry) * n_entries) - 1,
        .base = (uint64_t) gdt,
    };
    asm volatile("lgdt %0" : : "m"(gp));
}

void reload_segment_registers(uint16_t cs_selector, uint16_t ds_selector) {
    asm volatile(".intel_syntax noprefix\n\t"
                 "push %0\n\t"
                 "lea rax, [rip + 1f]\n\t"
                 "push rax\n\t"
                 "retfq\n\t"
                 "1:\n\t"
                 "mov ax, %1\n\t"
                 "mov ds, ax\n\t"
                 "mov es, ax\n\t"
                 "mov fs, ax\n\t"
                 "mov gs, ax\n\t"
                 "mov ss, ax\n\t"
                 ".att_syntax prefix\n\t"
                 :
                 : "r"((uint64_t) cs_selector), "r"(ds_selector)
                 : "rax", "ax", "memory");
}

void gdt_init(struct gdt_entry *gdt, struct tss *tss) {
    gdt_set_gate(gdt, 0, 0, 0, 0, 0); // Null

    gdt_set_gate(gdt, 1, 0, 0xFFFFFFFF, ACCESS_CODE_RING0, GRAN_CODE);
    gdt_set_gate(gdt, 2, 0, 0xFFFFFFFF, ACCESS_DATA_RING0, GRAN_DATA);
    gdt_set_gate(gdt, 5, 0, 0xFFFFFFFF, ACCESS_CODE_RING3, GRAN_CODE);
    gdt_set_gate(gdt, 6, 0, 0xFFFFFFFF, ACCESS_DATA_RING3, GRAN_DATA);

    // TSS (occupies entries 3 and 4)
    gdt_set_tss((struct gdt_entry_tss *) &gdt[3], (uint64_t) tss,
                sizeof(struct tss) - 1);

    tss->io_map_base = sizeof(struct tss);

    gdt_load(gdt, GDT_ENTRIES);
    tss->rsp0 = (uint64_t) hugepage_alloc_pages(8);
    if (!tss->rsp0)
        k_panic("GDT TSS RSP0 stack allocation failed!\n");

    reload_segment_registers(GDT_KERNEL_CODE, GDT_KERNEL_DATA);

    asm volatile("ltr %w0" : : "r"(0x18)); // TSS selector
}

void gdt_install(void) {
    struct gdt_entry *gdt =
        kmalloc_aligned(sizeof(struct gdt_entry) * GDT_ENTRIES, 64);
    struct tss *tss = kmalloc_aligned(sizeof(struct tss), 64);
    if (!gdt || !tss)
        return;

    memset(tss, 0, sizeof(struct tss));
    memset(gdt, 0, sizeof(struct gdt_entry) * GDT_ENTRIES);

    gdt_init(gdt, tss);

    return;
}
